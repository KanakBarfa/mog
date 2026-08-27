---- MODULE BookMatching ----

(***************************************************************************)
(* Bounded formal model of mog's matching semantics (RFC-0001).            *)
(*                                                                         *)
(* State: FIFO queues per (side, price) holding order entries, plus an     *)
(* aggregate mirror `agg` that actions update through their own arithmetic *)
(* - exactly how the engine keeps ladder aggregates separate from queue    *)
(* contents. Conservation between the two is therefore a real invariant,   *)
(* not a tautology.                                                        *)
(*                                                                         *)
(* Actions mirror engine operations:                                       *)
(*   Rest       ~ decide()'s rest path (refuses to cross, appends tail)    *)
(*   Cancel     ~ book cancel/remove by ref                                *)
(*   Deplete    ~ apply_external / consume_level (partial head fills)      *)
(*   MarketTake ~ cross() against full levels priced through a bound      *)
(*                                                                         *)
(* Price-time priority is enforced structurally: only level-head entries   *)
(* are ever consumed.                                                      *)
(***************************************************************************)

EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Refs,      \* set of order refs
          Prices,    \* set of price ticks per side
          MaxQty     \* maximum resting quantity per order

VARIABLES book,  \* [side][price] -> Seq([ref |-> ..., qty |-> ...]) FIFO heads first
          agg    \* [side][price] -> total quantity, maintained independently

Vars == <<book, agg>>

SideSet == {"B", "S"}

Opp(s) == IF s = "B" THEN "S" ELSE "B"

Entry(ref, q) == [ref |-> ref, qty |-> q]

Min(a, b) == IF a <= b THEN a ELSE b

\* Recursion-free by construction: entries per level are capped by |Refs|,
\* so the CASE arms cover every reachable sequence length under MC.cfg.
SumSeq(seq) ==
    CASE Len(seq) = 0 -> 0
    [] Len(seq) = 1 -> seq[1].qty
    [] Len(seq) = 2 -> seq[1].qty + seq[2].qty
    [] Len(seq) = 3 -> seq[1].qty + seq[2].qty + seq[3].qty

EmptyBook == [s \in SideSet |-> [p \in Prices |-> <<>>]]
EmptyAgg  == [s \in SideSet |-> [p \in Prices |-> 0]]

Init == /\ book = EmptyBook
        /\ agg  = EmptyAgg

(***************************************************************************)
(* True when ref rests nowhere in the book.                                *)
(*****************************************************************************)
RefUnused(r) ==
    \A s \in SideSet : \A p \in Prices :
        \A i \in 1..Len(book[s][p]) : book[s][p][i].ref # r

(***************************************************************************)
(* Best touch prices; sentinel 0 (never a tick) when a side is empty.      *)
(* The nonempty set is computed first so `book` is only ever applied at    *)
(* prices in its domain - TLC gives no short-circuit guarantee that would  *)
(* protect the naive `book[p] /= <<>> \/ p = 0` form.                      *)
(*****************************************************************************)
BestBuy ==
    LET ne == {x \in Prices : book["B"][x] /= <<>>}
    IN IF ne = {} THEN 0
       ELSE CHOOSE p \in ne : \A x \in ne : x <= p

BestAsk ==
    LET ne == {x \in Prices : book["S"][x] /= <<>>}
    IN IF ne = {} THEN 0
       ELSE CHOOSE p \in ne : \A x \in ne : x >= p

WouldCross(s, p) ==
    CASE s = "B" -> BestAsk # 0 /\ BestAsk <= p
    [] s = "S" -> BestBuy # 0 /\ BestBuy >= p

(***************************************************************************)
(* Rest(ref, s, p, q): append at the tail of its level iff not crossing,   *)
(* mirroring decide()'s post-only/limit gate.                              *)
(*****************************************************************************)
Rest(ref, s, p, q) ==
    /\ ref \in Refs
    /\ p \in Prices
    /\ q \in 1..MaxQty
    /\ ~WouldCross(s, p)
    /\ RefUnused(ref)
    /\ book' = [book EXCEPT ![s][p] = Append(book[s][p], Entry(ref, q))]
    /\ agg'  = [agg EXCEPT ![s][p] = agg[s][p] + q]

(***************************************************************************)
(* Cancel(ref): remove an existing order from wherever it rests.           *)
(*****************************************************************************)
(* Append adds ONE element to a sequence; splicing out a middle entry      *)
(* needs concatenation \o of the slices around it.                         *)
RemoveAt(seq, i) ==
    CASE i = 1 -> Tail(seq)
    [] i = Len(seq) -> SubSeq(seq, 1, Len(seq) - 1)
    [] OTHER -> SubSeq(seq, 1, i - 1) \o SubSeq(seq, i + 1, Len(seq))

CancelIdx(s, p, i) ==
    /\ book' = [book EXCEPT ![s][p] = RemoveAt(book[s][p], i)]
    /\ agg'  = [agg EXCEPT ![s][p] = agg[s][p] - book[s][p][i].qty]

Cancel(ref) ==
    \E s \in SideSet :
        \E p \in Prices :
            \E i \in 1..Len(book[s][p]) :
                /\ book[s][p][i].ref = ref
                /\ CancelIdx(s, p, i)

(***************************************************************************)
(* Deplete(s, p, q): consume up to q units from this level's head.         *)
(* Partial head fills keep the remainder AT THE HEAD - same ref, same      *)
(* position - exactly like the engine's reduce(); only full consumption    *)
(* drops the entry.                                                        *)
(*****************************************************************************)
Deplete(s, p, q) ==
    /\ p \in Prices
    /\ q \in 1..MaxQty
    /\ book[s][p] /= <<>>
    /\ LET take == Min(q, Head(book[s][p]).qty)
           h    == Head(book[s][p])
       IN /\ book' = IF take = h.qty
                     THEN [book EXCEPT ![s][p] = Tail(book[s][p])]
                     ELSE [book EXCEPT ![s][p] =
                             <<Entry(h.ref, h.qty - take)>> \o Tail(book[s][p])]
          /\ agg'  = [agg EXCEPT ![s][p] = agg[s][p] - take]

(***************************************************************************)
(* MarketTake(s, bound): remove ENTIRE levels priced through the bound on  *)
(* the opposite side - cross() semantics with remainder discarded, so no   *)
(* crossed state is reachable. Levels clear atomically one at a time.      *)
(*****************************************************************************)
TakeLevel(s, p) ==
    /\ book' = [book EXCEPT ![Opp(s)][p] = <<>>]
    /\ agg'  = [agg EXCEPT ![Opp(s)][p] = 0]

MarketTakeStep(s, bound) ==
    \E p \in {x \in Prices :
                  book[Opp(s)][x] /= <<>> /\
                  IF s = "B" THEN x <= bound ELSE x >= bound} :
        TakeLevel(s, p)

MarketTake(s, bound) ==
    /\ bound \in Prices
    /\ MarketTakeStep(s, bound)

Next ==
    \/ \E ref \in Refs, s \in SideSet, p \in Prices, q \in 1..MaxQty :
            Rest(ref, s, p, q)
    \/ \E ref \in Refs : Cancel(ref)
    \/ \E s \in SideSet, p \in Prices, q \in 1..MaxQty : Deplete(s, p, q)
    \/ \E s \in SideSet, bound \in Prices : MarketTake(s, bound)

(***************************************************************************)
(* Invariants.                                                             *)
(*****************************************************************************)
TypeOK ==
    /\ book \in [SideSet -> [Prices -> Seq([ref: Refs, qty: 1..MaxQty])]]
    /\ agg  \in [SideSet -> [Prices -> Int]]

Conservation ==
    \A s \in SideSet, p \in Prices : agg[s][p] = SumSeq(book[s][p])

NoCrossedBook ==
    ~(BestBuy # 0 /\ BestAsk # 0 /\ BestBuy >= BestAsk)

RefUnique ==
    \A s1 \in SideSet :
        \A p1 \in Prices :
            \A i \in 1..Len(book[s1][p1]) :
                \A s2 \in SideSet :
                    \A p2 \in Prices :
                        \A j \in 1..Len(book[s2][p2]) :
                            (s1 # s2 \/ p1 # p2 \/ i # j) =>
                                book[s1][p1][i].ref # book[s2][p2][j].ref

QtyPositive ==
    \A s \in SideSet, p \in Prices : agg[s][p] >= 0

Inv == /\ TypeOK
       /\ Conservation
       /\ NoCrossedBook
       /\ RefUnique
       /\ QtyPositive

Spec == Init /\ [][Next]_Vars

=============================================================================
