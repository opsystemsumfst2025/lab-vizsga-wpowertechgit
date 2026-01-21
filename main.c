#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_RESET "\x1b[0m"

#define NUM_TRADERS 3
#define BUFFER_SIZE 10
#define INITIAL_BALANCE 10000.0
#define SIMULATION_TIME -1 // -1 = végtelen (Ctrl+C), egyébként másodpercek
// 1. A piac processz write hivasa blokkolo ha a pipe puffere tele van, a gyerekfolyamat varakozni fog,
// amig a szulo el nem olvassa az adatokat a pipe-bol.
// 2. Condition variable : A trader szalak hasznalhatnak egy condition variable-t, hogy varakozzanak,
// amig uj arfolyam erkezik a bufferbe. Amikor a piac processz uj arfolyamot ir a bufferbe,
// jelzi a condition variable-t, hogy felkeltse a varakozo szalak figyelmet, igy nincs pazarlas.
// 3. a mutexet a pthread_mutex_unlock() hivasok vegzik el, hogy feloldjak a lezart mutexet,
// ha a szal a mutex birtokaban fagyna le (pl. segfault) a mutex lockolva maradna es minden mas szal blokkolva lenne (deadlock).
// 4. CTRL+C a terminalon egy teljes folyamatcsoportnak kuld SIGINT jelet,
// a kodban viszont kuldunk egy kill(market_pid, SIGTERM) ,
// jelet a piac folyamatnak hogy szepen lealljon es biztosan kilepjen a gyerek a szulo folyamatbol.
// 5. Valgrind futasanal hany byte veszik el tranzakcionkent ha nem szabaditjuk fel a tranzakciokat?
// minden tranzakcio 5(type) + 10(stock) + 4(quantity) + 8(price) + 8(next pointer) = 35 byte veszik el tranzakcionkent
// de felszabaditjuk szoval nincs baj.
// Transaction struktúra - láncolt lista

// TODO: Definiáld a Transaction struktúrát (láncolt lista)
// Tartalmazzon: type, stock, quantity, price, next pointer
typedef struct Transaction
{
    char type[5]; // "BUY" vagy "SELL"
    char stock[10];
    int quantity;
    double price;
    struct Transaction *next;
} Transaction;

// StockPrice struktúra
// TODO: Definiáld a StockPrice struktúrát
// Tartalmazzon: stock név, price
typedef struct
{
    char stock[10];
    double price;
} StockPrice;

// TODO: Globális változók
// - price_buffer tömb
// - buffer_count, buffer_read_idx, buffer_write_idx
// - wallet_balance, stocks_owned
// - mutex-ek (wallet, buffer, transaction)
// - condition variable
// - transaction_head pointer
// - running flag (volatile sig_atomic_t)
// - market_pid
// Globális változók
StockPrice price_buffer[BUFFER_SIZE];
int buffer_count = 0;
int buffer_read_idx = 0;
int buffer_write_idx = 0;

double wallet_balance = INITIAL_BALANCE;
int stocks_owned = 0;

pthread_mutex_t wallet_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

Transaction *transaction_head = NULL;

volatile sig_atomic_t running = 1;
pid_t market_pid = -1;
int pipe_fd[2];
time_t start_time;

// Transaction hozzáadása a láncolt listához
void add_transaction(const char *type, const char *stock, int quantity, double price)
{
    Transaction *new_trans = (Transaction *)malloc(sizeof(Transaction));
    if (new_trans == NULL)
    {
        perror("malloc");
        return;
    }

    strcpy(new_trans->type, type);
    strcpy(new_trans->stock, stock);
    new_trans->quantity = quantity;
    new_trans->price = price;

    pthread_mutex_lock(&transaction_mutex);
    new_trans->next = transaction_head;
    transaction_head = new_trans;
    pthread_mutex_unlock(&transaction_mutex);
}

// Tranzakciók kiírása
// TODO: Implementáld a print_transactions függvényt
// Járd végig a láncolt listát mutex lock alatt
// Írd ki az összes tranzakciót
void print_transactions()
{
    pthread_mutex_lock(&transaction_mutex);

    printf("\n========================================\n");
    printf("  TRANZAKCIOK LISTAJA\n");
    printf("========================================\n");

    Transaction *current = transaction_head;
    int count = 0;

    while (current != NULL)
    {
        const char *color = (strcmp(current->type, "BUY") == 0) ? ANSI_COLOR_GREEN : ANSI_COLOR_RED;
        printf("[%d] %s%s %s%s - Mennyiseg: %d, Ar: %.2f $\n",
               ++count, color, current->type, current->stock, ANSI_COLOR_RESET,
               current->quantity, current->price);
        current = current->next;
    }

    if (count == 0)
    {
        printf("Nincs tranzakcio.\n");
    }

    printf("========================================\n\n");
    pthread_mutex_unlock(&transaction_mutex);
}

// Tranzakciók felszabadítása
// TODO: Implementáld a free_transactions függvényt
// FONTOS: Járd végig a listát és free()-zd az összes elemet
// Ez kell a Valgrind tiszta kimenethez!
void free_transactions()
{
    pthread_mutex_lock(&transaction_mutex);

    Transaction *current = transaction_head;
    while (current != NULL)
    {
        Transaction *temp = current;
        current = current->next;
        free(temp);
    }
    transaction_head = NULL;

    pthread_mutex_unlock(&transaction_mutex);
}

// Signal handler (SIGINT - Ctrl+C)
// TODO: Signal handler (SIGINT)
// Állítsd be a running flag-et 0-ra
// Küldj SIGTERM-et a market_pid folyamatnak (kill függvény)
// Ébreszd fel a szálakat (pthread_cond_broadcast)
void sigint_handler(int sig)
{
    (void)sig;
    printf("\n\n[SIGNAL] SIGINT kapva - leallitas...\n");
    running = 0;

    // Piac folyamat leállítása
    if (market_pid > 0)
    {
        kill(market_pid, SIGTERM);
    }

    // Szálak ébresztése
    pthread_cond_broadcast(&buffer_cond);
}

// Időzítő handler (SIGALRM)
void sigalrm_handler(int sig)
{
    (void)sig;
    printf("\n\n[TIMER] Szimulacio ideje lejart!\n");
    running = 0;

    // Piac folyamat leállítása
    if (market_pid > 0)
    {
        kill(market_pid, SIGTERM);
    }

    // Szálak ébresztése
    pthread_cond_broadcast(&buffer_cond);
}

// Piac folyamat
// TODO: Piac folyamat függvénye
// Végtelen ciklusban:
// - Generálj random részvénynevet és árat
// - Írás a pipe_fd-re (write)
// - sleep(1)
void market_process(int write_fd)
{
    const char *stocks[] = {"AAPL", "GOOG", "TSLA", "MSFT", "AMZN", "NVDA", "META", "AVGO", "NFLX", "INTC"};
    size_t num_stocks = sizeof(stocks) / sizeof(stocks[0]);

    srand(time(NULL) ^ getpid());
    close(pipe_fd[0]); // Olvasó oldalt zárjuk

    while (running)
    {
        char buffer[32];
        int stock_idx = rand() % num_stocks;
        double price = 100.0 + (rand() % 2000);

        snprintf(buffer, sizeof(buffer), "%s %.2f\n", stocks[stock_idx], price);

        ssize_t written = write(write_fd, buffer, strlen(buffer));
        if (written < 0)
        {
            break;
        }

        sleep(1);
    }

    close(write_fd);
    exit(0);
}

// Kereskedő szál
// TODO: Kereskedő szál függvénye
// Végtelen ciklusban:
// - pthread_cond_wait amíg buffer_count == 0
// - Kivesz egy árfolyamot a bufferből (mutex alatt!)
// - Kereskedési döntés (random vagy stratégia)
// - wallet_balance módosítása (MUTEX!!!)
// - add_transaction hívás
void *trader_thread(void *arg)
{
    int trader_id = *(int *)arg;
    free(arg);

    srand(time(NULL) ^ pthread_self());

    while (running)
    {
        StockPrice stock_price;

        // Buffer ellenőrzése és várakozás
        pthread_mutex_lock(&buffer_mutex);
        while (buffer_count == 0 && running)
        {
            pthread_cond_wait(&buffer_cond, &buffer_mutex);
        }

        if (!running)
        {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }

        // Árfolyam kivétele
        stock_price = price_buffer[buffer_read_idx];
        buffer_read_idx = (buffer_read_idx + 1) % BUFFER_SIZE;
        buffer_count--;
        pthread_mutex_unlock(&buffer_mutex);

        // Kereskedési döntés
        int decision = rand() % 3; // 0: BUY, 1: SELL, 2: HOLD
        int quantity = 1 + (rand() % 5);

        pthread_mutex_lock(&wallet_mutex);

        if (decision == 0)
        { // BUY
            double cost = stock_price.price * quantity;
            if (wallet_balance >= cost)
            {
                wallet_balance -= cost;
                stocks_owned += quantity;
                printf(ANSI_COLOR_GREEN "[TRADER %d] BUY %d %s @ %.2f $ | Egyenleg: %.2f $ | Reszveny: %d\n" ANSI_COLOR_RESET,
                       trader_id, quantity, stock_price.stock, stock_price.price,
                       wallet_balance, stocks_owned);

                pthread_mutex_unlock(&wallet_mutex);
                add_transaction("BUY", stock_price.stock, quantity, stock_price.price);
            }
            else
            {
                pthread_mutex_unlock(&wallet_mutex);
            }
        }
        else if (decision == 1 && stocks_owned >= quantity)
        { // SELL
            double revenue = stock_price.price * quantity;
            wallet_balance += revenue;
            stocks_owned -= quantity;
            printf(ANSI_COLOR_RED "[TRADER %d] SELL %d %s @ %.2f $ | Egyenleg: %.2f $ | Reszveny: %d\n" ANSI_COLOR_RESET,
                   trader_id, quantity, stock_price.stock, stock_price.price,
                   wallet_balance, stocks_owned);

            pthread_mutex_unlock(&wallet_mutex);
            add_transaction("SELL", stock_price.stock, quantity, stock_price.price);
        }
        else
        {
            pthread_mutex_unlock(&wallet_mutex);
        }

        usleep(10000); // 10ms késleltetés
    }

    printf("[TRADER %d] Leallitas...\n", trader_id);
    return NULL;
}

int main()
{
    pthread_t traders[NUM_TRADERS];

    printf("========================================\n");
    printf("  WALL STREET - PARHUZAMOS TOZSDE\n");
    printf("========================================\n");
    printf("Kezdo egyenleg: %.2f $\n", INITIAL_BALANCE);
    printf("Kereskedok szama: %d\n", NUM_TRADERS);

    if (SIMULATION_TIME == -1)
    {
        printf("Mod: Vegtelen (Ctrl+C a leallitashoz)\n");
    }
    else
    {
        printf("Szimulacio ideje: %d masodperc\n", SIMULATION_TIME);
    }

    printf("========================================\n\n");

    // Start time rögzítése
    start_time = time(NULL);

    // Signal handler regisztrálása
    // TODO: Signal handler regisztrálása
    // signal(SIGINT, ...);
    signal(SIGINT, sigint_handler);
    signal(SIGALRM, sigalrm_handler);

    // Ha van időkorlát, állíts be timer-t
    if (SIMULATION_TIME > 0)
    {
        alarm(SIMULATION_TIME);
    }

    // Pipe létrehozása
    // TODO: Pipe létrehozása
    // pipe(pipe_fd);
    if (pipe(pipe_fd) == -1)
    {
        perror("pipe");
        return 1;
    }

    // Fork - Piac folyamat indítása
    // TODO: Fork - Piac folyamat indítása
    // market_pid = fork();
    // Ha gyerek (== 0): piac folyamat
    // Ha szülő: kereskedő szálak indítása
    market_pid = fork();

    if (market_pid == -1)
    {
        perror("fork");
        return 1;
    }

    if (market_pid == 0)
    {
        // Gyerek folyamat - Piac
        close(pipe_fd[0]);
        market_process(pipe_fd[1]);
        exit(0);
    }

    // Szülő folyamat
    close(pipe_fd[1]); // Író oldalt zárjuk

    // Kereskedő szálak indítása
    // TODO: Kereskedő szálak indítása (pthread_create)
    // for ciklus, malloc az id-nak
    for (int i = 0; i < NUM_TRADERS; i++)
    {
        int *id = malloc(sizeof(int));
        *id = i + 1;
        if (pthread_create(&traders[i], NULL, trader_thread, id) != 0)
        {
            perror("pthread_create");
            free(id);
        }
    }

    // Master ciklus - Pipe olvasása
    // TODO: Master ciklus
    // Olvasd a pipe-ot (read)
    // Parse-old az árakat
    // Tedd be a bufferbe (mutex alatt!)
    // pthread_cond_broadcast
    char buffer[1024];
    ssize_t bytes_read;
    char temp[1024] = "";

    while (running && (bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0)
    {
        buffer[bytes_read] = '\0';
        strcat(temp, buffer);

        char *line_end;
        while ((line_end = strchr(temp, '\n')) != NULL)
        {
            *line_end = '\0';

            char stock[10];
            double price;
            if (sscanf(temp, "%s %lf", stock, &price) == 2)
            {
                pthread_mutex_lock(&buffer_mutex);

                if (buffer_count < BUFFER_SIZE)
                {
                    strcpy(price_buffer[buffer_write_idx].stock, stock);
                    price_buffer[buffer_write_idx].price = price;
                    buffer_write_idx = (buffer_write_idx + 1) % BUFFER_SIZE;
                    buffer_count++;

                    pthread_cond_broadcast(&buffer_cond);
                }

                pthread_mutex_unlock(&buffer_mutex);
            }

            memmove(temp, line_end + 1, strlen(line_end + 1) + 1);
        }
    }

    close(pipe_fd[0]);

    // Cleanup
    // TODO: Cleanup
    // pthread_join a szálakra
    // waitpid a Piac folyamatra
    printf("\n[RENDSZER] Varakozas a szalakra...\n");
    for (int i = 0; i < NUM_TRADERS; i++)
    {
        pthread_join(traders[i], NULL);
    }

    printf("[RENDSZER] Varakozas a Piac folyamatra...\n");
    waitpid(market_pid, NULL, 0);

    // Végső kiírások
    time_t end_time = time(NULL);
    double elapsed = difftime(end_time, start_time);

    printf("\n========================================\n");
    printf("  VEGSO JELENTES\n");
    printf("========================================\n");
    printf("Futasi ido: %.0f masodperc\n", elapsed);
    printf("Vegso egyenleg: %.2f $\n", wallet_balance);
    printf("Birtokolt reszveny: %d db\n", stocks_owned);

    double total_value = wallet_balance + (stocks_owned * 500.0); // Átlagár becslés
    double profit = total_value - INITIAL_BALANCE;
    printf("Becsult portfolio ertek: %.2f $\n", total_value);
    printf("Profit/Veszteseg: %.2f $ (%.2f%%)\n",
           profit, (profit / INITIAL_BALANCE) * 100);
    printf("========================================\n");

    print_transactions();
    free_transactions();

    pthread_mutex_destroy(&wallet_mutex);
    pthread_mutex_destroy(&buffer_mutex);
    pthread_mutex_destroy(&transaction_mutex);
    pthread_cond_destroy(&buffer_cond);

    printf(ANSI_COLOR_RESET "[RENDSZER] Sikeres leallitas.\n");
    return 0;
}