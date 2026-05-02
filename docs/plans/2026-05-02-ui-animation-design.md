# UI Animation Design

## Goal

Add lightweight, consistent UI transitions for three existing interaction surfaces without changing the surrounding workflows:

- Settings overlay open and close
- Settings page transitions between General and Audio tabs
- Right-side workspace panel open, close, and panel swaps

The animation language should feel restrained: short fades, small positional shifts, and minor scale change on the settings card.

## Scope

### Settings overlay

- Opening: dim background fades in while the settings card fades in, shifts upward slightly, and scales from a near-rest state.
- Closing: the same motion runs in reverse, and the overlay is hidden only after the animation completes.
- Re-entry: if the user reopens during a close animation, the target direction flips without creating a second overlay instance.

### Settings tabs

- Switching between General and Audio animates the page content instead of hard-hiding controls instantly.
- The outgoing page fades and slides slightly left.
- The incoming page fades and slides slightly in from the right.
- Plugin mode continues to force the General tab only, with no empty transition path.

### Right-side panels

- Showing a panel animates the workspace split instead of instantly snapping the panel container width.
- Hiding a panel reverses the width and content alpha.
- Switching between parameter and voicebank panels uses a single transition, keeping the container visible while content swaps.

## Implementation plan

### Shared approach

- Use small timer-driven state machines local to the components that own the affected visuals.
- Avoid introducing a global animation framework.
- Keep layout ownership where it already exists:
  - MainComponent owns settings overlay visibility intent.
  - SettingsOverlay and SettingsComponent own settings rendering progress.
  - WorkspaceComponent owns panel container width progression.

### Settings overlay changes

- Add animation state and a timer to SettingsOverlay.
- Expose open and close entry points instead of direct visibility toggles.
- Render dim amount and card transform from normalized progress.
- Close requests from background click, escape, or close button start the reverse animation instead of hiding immediately.

### Settings tab changes

- Group General and Audio controls into two page containers.
- Animate page alpha and horizontal offset based on an active and previous tab state.
- Keep tab button styling updates immediate so input feedback stays sharp.

### Workspace panel changes

- Track an animated panel width in WorkspaceComponent.
- Keep PanelContainer layout simple; drive the container bounds from WorkspaceComponent.
- Add panel alpha/offset handling inside PanelContainer so the visible panel eases with the width animation.

## Constraints

- Keep animation durations roughly within 140-180 ms.
- Avoid adding dependencies or changing persistent state formats.
- Preserve current behavior under repeated rapid toggles.
- Resizing during animation must settle into the correct final bounds.

## Validation

- Build Release target successfully.
- Manually verify:
  - settings overlay open and close
  - tab transition in settings
  - parameter panel toggle
  - voicebank panel toggle
  - switching directly between parameter and voicebank panels