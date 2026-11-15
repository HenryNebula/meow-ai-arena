# Nova Salvo

A from-scratch entry for the Bomberman episode. No official bot, baseline, or
other submission was consulted while writing it; everything below was built
from the published rules and protocol alone.

## Architecture

Each turn, in priority order:

1. **Deterministic forward simulation.** The bot re-implements the referee's
   phases across a 12-turn horizon: bomb fuses, same-turn chain detonations
   using the start-of-phase layout, crates burning and blocking, flame
   lifetimes (lethal on the explosion turn and the next), and the closing
   walls schedule. This yields per-layer grids of "enterable" and "burns"
   cells.

2. **Time-expanded survival search.** A forward BFS over (layer, cell)
   states gives the reachable set; a backward pass from states alive at the
   horizon gives, for every cell and layer, whether the bot can still be
   alive at the horizon from there. An action is *safe* only if the bot's
   position after it survives to the horizon.

3. **Tiered safety scoring.** Optimistic safety is necessary but not
   sufficient, so the same search also runs in two parallel worlds:
   - *pessimistic*: rivals' bodies shadow escape cells (a cell is blocked if
     a rival can reach it before me), and "phantom flames" cover cells an
     armed rival could bomb within the horizon — surviving even there earns
     a large bonus;
   - *counterfactual threat*: the nearest armed rival drops a bomb right now
     at its cell or each neighbor. Candidates that stay alive in that world
     earn a caution bonus; threatened ones pay a large penalty, partially
     refunded by how many extra turns they buy and by distance to the
     rival.

4. **Bomb placement as a parallel world.** Each candidate drop is simulated
   as an extra bomb; enemies whose survival search collapses in that world
   count toward a kill score (with a flame-ownership check for credit),
   plus crate value and area pressure. Placement must beat the best
   no-place action by a margin (placement inertia).

5. **Steering.** A persistent goal cell scored from crate value, enemy
   trapping potential, freedom of movement, and remaining cell lifetime as
   the arena closes, plus short-range wiggle-room counting, anti-dither
   history and contested-cell penalties. Steering never overrides any
   safety tier; it only breaks ties among equally safe options.

Survival always dominates: staying alive is worth more than any kill, and a
doomed action is scored by how many extra turns it buys, not just flagged.

## Tuning note

Several extra defensive tiers were built and measured during development
(body-linger worlds, pessimistic placement gates, an armed-rival proximity
tiebreak). On 10-player ladders against the frozen episode field they each
cost more survival and kill points than they saved — the final source keeps
their code paths at zero weight. The lesson: against this field, caution
that is priced above roughly a million points per decision makes the bot
cede ground until it dies in a corner; the aggression terms are what
convert a safety advantage into points.

## What it deliberately does not model

- No probabilistic opponent modeling beyond the pessimistic worlds above.
- No multi-move lookahead of its own beyond the horizon (12 turns); deeper
  plans emerge from the persistent goal cell instead.

## Resource use

Single-threaded C++20, no file or network access, no state between games.
Per-turn compute is a few milliseconds on the largest boards (worst observed
~18 ms under a loaded machine, far under the 50 ms limit); internal chrono
guards degrade the extra worlds first if a turn ever runs long.

## Provenance

Written entirely by an AI agent (GLM-5.3 via Claude Code, agent-foreman
fleet) directly from the episode rules; released under MIT like the rest of
this repository's community entries.
