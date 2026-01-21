[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/K8PRfra2)
# Wall Street – A Párhuzamos Tőzsde

## Feladat
Írj egy C programot, ami szimulál egy egyszerűsített tőzsdei kereskedő rendszert. A rendszer fogadja az árfolyamokat, párhuzamosan elemzi őket, közös számláról kereskedik, és naplózza az eseményeket.

## Architektúra

### 1. Árfolyam Generátor (Fork + Pipe)
- A főprogram indítson egy gyermek folyamatot (`fork`)
- A gyerek a "Piac": másodpercenként generál véletlenszerű részvényárakat
- Formátum: `"AAPL 150"`, `"GOOG 2800"`
- Névtelen csövön (`pipe`) küldi az adatokat a szülőnek
- Technológia: `fork()`, `pipe()`, `write()`

### 2. Kereskedő Motor (Pthreads + Mutex)
- A szülő folyamat olvassa a pipe-ot
- Bejövő árfolyamokat bufferbe teszi
- Indítson N db kereskedő szálat (`pthread_create`)
- A szálak versengenek a feladatokért
- **Kritikus szakasz**: közös `wallet_balance` és `stocks_owned`
- Védelem: `pthread_mutex_t`

### 3. Szinkronizáció (Condition Variable)
- Ha nincs új árfolyam, a szálak alusznak (NEM busy-wait!)
- Master új adat esetén: `pthread_cond_signal` vagy `pthread_cond_broadcast`
- Szálak: `pthread_cond_wait` blokkol

### 4. Memóriakezelés (Dynamic Memory)
- Minden tranzakciót dinamikusan menteni láncolt listába
- Használat: `malloc`, `free`
- Kilépéskor mindent fel kell szabadítani (memóriaszivárgás tiltva!)

### 5. Vészfék (Signals)
- Program végtelenített futás, Ctrl+C (SIGINT) állítja le
- Signal handler:
  - Üzenjen a Piac folyamatnak (`kill` SIGTERM)
  - Jelezzen a szálaknak (join)
  - Írja ki a végső egyenleget és tranzakciókat
  - Takarítás és `exit(0)`

## Követelmények

**Kötelező elemek:**
- [x] `fork()` és `pipe()` használata
- [x] Legalább 2 pthread szál
- [x] `pthread_mutex_t` a kritikus szakaszokhoz
- [x] `pthread_cond_t` vagy szemafor
- [x] SIGINT signal handler
- [x] `malloc` és `free` (láncolt lista)
- [x] Valgrind tiszta kimenet

**Tiltott:**
- Busy-wait ciklusok szinkronizációhoz
- Memóriaszivárgás
- Race condition a wallet/stocks változókon

## Fordítás és Futtatás

```bash
make            # Fordítás
make run        # Futtatás
make valgrind   # Memória ellenőrzés
```

Ctrl+C megnyomása állítja le a programot.

## Értékelési Kérdések (Védéshez)

1. **A Piac processz write hívása blokkoló vagy nem blokkoló? Mi történik, ha a cső megtelik?**

2. **Miért kellett Condition Variable-t használnod? Miért nem volt elég egy `while(ures)` ciklus?**

3. **Mutasd meg, hol zárod le a Mutexet! Mi történne, ha a `wallet_balance` módosítása előtt lefagyna a szál?**

4. **Amikor megnyomod a Ctrl+C-t, a gyerek folyamat magától hal meg, vagy neked kell lelőnöd? Miért?**

5. **Valgrind futtatás! Ha a láncolt listát nem szabadítod fel, hány byte veszik el tranzakciónként?**

## Javasolt Lépések

1. Kezdd a struktúrák és globális változók definiálásával
2. Implementáld a pipe és fork logikát
3. Készítsd el a szál függvényt
4. Add hozzá a mutex védelmeket
5. Implementáld a condition variable szinkronizációt
6. Készítsd el a signal handlert
7. Tesztelj Valgrind-dal
