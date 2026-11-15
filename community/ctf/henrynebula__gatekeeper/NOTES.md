# Gatekeeper — notes

Written from scratch (no reference to any other bot's source) for episode 3.
Single-file C++20, standard library only.

## Core ideas

1. **Precomputed geometry.** At init the bot builds BFS distance fields from
   every team's base and every flag spot (walls block). All later decisions
   are lookups into these fields, so a turn costs microseconds.

2. **Exact respawn prediction.** The engine's spawn rule is deterministic:
   spawn cells are the 9 base cells sorted by (distance to map centre, then a
   tiebreak), processed in unit-id order against live occupancy. The bot
   mirrors it, so it knows exactly which enemy units reappear this turn —
   immune units are skipped as attack targets, and fresh spawns near a
   contested flag are anticipated.

3. **Survival-gated pickups (the name-giving part).** Damage is 34 and a
   chaser within strike range keeps pace with a carrier, landing a hit every
   turn: any carrier needing more than two turns home dies before delivering.
   So the bot only picks up when the carry can actually survive — no enemies
   within range 2 of the flag cell, or at most one and two friends can drop
   it in time. Otherwise the unit **stands on the flag cell anyway**: an
   uncarried flag under a unit's feet blocks every enemy pickup, so the
   "declined" grab still denies the flag while the unit fights for the spot.

4. **Spot farming with local support only.** Free units spread over the
   nearest empty flag spots (the central race zone gets a small priority
   bonus), turning random respawns into instant grabs. Escort and guard
   duties are capped at 4 steps of reach: units already nearby help a
   threatened carrier or contested fetcher; distant units keep their spot
   coverage instead of being pulled across the map.

5. **Assignment as optimization.** Each turn an injective unit→flag
   assignment is solved optimally (3 units, few flags — brute force with
   pruning), trading travel distance, carry-home exposure scaled by the
   number of teams, respawn races (concede a flag an enemy clearly reaches
   first), and a fallback value for not fetching at all.

6. **Micro.** Movement scores all five steps against the goal field with
   enemy-threat penalties, friendly-cell reservations (so our units never
   block each other), and a hold-and-fight bonus near contested goals in
   games with 4+ teams (2-player games stay tempo-first). Actions prefer
   pickups, then the best attack target: enemy carriers, kill shots, and
   focus fire on targets a teammate already hit.

## Results

`python ctf/challenge.py` vs the 11 official episode-3 bots placed this bot
8th of 12 with rating +4 (baseline = 0), strongest in 8-player games (+25)
and 2-player (−10). The three development checkpoints moved −45 → −22 → +4,
driven mostly by the survival gate (carrier conversion rate roughly doubled)
and the local-support caps (spot coverage stopped churning).
