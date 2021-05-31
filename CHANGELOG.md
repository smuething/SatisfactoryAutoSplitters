# Changelog

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
