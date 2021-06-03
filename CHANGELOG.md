# Changelog

## 0.3.7

### Changes

- Re-enable hook that keeps Satisfactory from crashing when upgrading splitters to auto splitters
- Completely rework savegame upgrade from 0.2.0 and older: Just dismantle old autosplitters and re-build them from scratch with
  the hologram.
- Fix a crash when attaching conveyors to an autosplitter that had all belts removed. Someday I'll remember the order of arguments
  in TArray::Init()...
- Salvage cases where the fixed precision arithmetic cannot perfectly assign output rates by distributing the remainder
  proportionally. Shouldn't be a big deal in terms of fairness, but might lead to fairly long cycle times.
- Reduce the risk of multiple threads stepping on each other's toes when the factory tick and grab output run simultaneously. I don't
  know whether that can actually happen, but let's build in some basic safeguards. I'd really like to avoid a full-blown lock on each
  splitter, though.
## 0.3.5

### Changes

- Make upgrade process more robust by comparing world coordinates when matching components.
- Delay all modifying operations until all `BeginPlay()` calls during the initial savegame initialization have
  finished.
- Add configuration system based on SML's config support.
- Add a fall-back upgrade method that simply removes all splitters when upgrading from 0.2.0 or older. Can be
  enabled through configuration.
- Make input rate configurable and fix some problems with propagation of auto state to upstream splitter.
- Take into account overclocking settings of downstream factories when calculating distribution shares. Limitations:
  - The calculation is based on fixed precision arithmetic, for now limited to a precision of 0.01%.
  - The allocation is rather finicky, as it will bail out if the shares cannot be precisely allocated without any remainder. 
  - The allocation is only updated when interacting with the network by building or removing components or showing the splitter
    UI. It will not auto-detect changes to the overclock rate.
- First internal changes to pave the way for multiplayer support.
## 0.3.0

### Changes
- Regular Splitters can now be upgraded to Auto Splitters with the build gun. Yay!
- The rate calculations are now based on fixed point arithmetic using the items/min metric.
  As a result, cycle times should no longer explode when playing with different rates. The number of fractional digits is limited to 3.
- Added awesome icons by Deantendo (Deantendo#4265 on the Modding discord) for the mod, the milestone
  and the recipe. Thank you so much!
- The UI now shows (and lets you enter) the items/min for each output. For now, the first splitter
  always assumes a completely full belt.
- The UI shows the assumed input rate that is used for the calculations.
- After changing a number, you have to hit Enter to make it effective. If the splitter network cannot
  calculate an item distribution based on your number, it will just revert to the old number.

### Caveats
- There is a known bug, where the boxes around splitters and merge don't disappear after upgrading to
  an Auto Splitter. As a workaround, start building something else like a belt and exit the build mode again.

## 0.2.0

- Fixed positioning of splitters in relation to vanilla splitters and mergers.

## 0.1.0

- Initial release
