# Decal Seam Fix

An SKSE plugin for Skyrim Special Edition / Anniversary Edition that fixes spell and impact marks (scorch marks, frost, blood and other ground decals) being cut off with a hard straight edge on the ground.

In vanilla Skyrim, terrain is split into cells, and each cell into four quadrants, each with its own mesh. A decal on the ground is built as separate pieces, one per quadrant it touches. Several engine bugs lose some of those pieces, so you see half a scorch mark ending in a straight line along a quadrant or cell border. This plugin fixes all four causes:

1. **Pieces dropped near other decals.** When a new decal lands close to an existing one, the engine may silently reject the piece for one quadrant and keep the others. Pieces on terrain are now always placed.
2. **Pieces lost after loading a save.** The engine assigns every piece to the cell where the hit happened, even when the piece lies on a neighbouring cell's terrain. The save keeps only pieces that belong to the cell they lie on, so after loading, decals near cell borders come back cut in half. Each piece is now assigned to the cell whose terrain it actually lies on, so it is saved and restored correctly.
3. **Neighbouring terrain not found after walking back.** This is the main cause. When you leave an area and come back, the game reuses recently unloaded cells from its cell buffer. The physics bodies of their terrain come back without the link to their 3D mesh. The engine uses that link to place decal pieces, so decals stop crossing into those cells until you reload the game. The plugin watches cells that come back from the buffer and restores the missing links. If a piece still arrives without a terrain mesh, the plugin places it on the quadrants the decal reaches.
4. **Old decals removed by halves.** When a cell unloads, the game removes the decal pieces on its terrain but leaves the pieces of the same decal on the neighbouring cell. When the game removes one piece of a decal, the plugin now hides the rest of it too, so decals disappear as a whole.

## Requirements

- SKSE64
- Address Library for SKSE Plugins
- Tested on Skyrim SE 1.6.1170

## Compatibility

- No ESP and no edited game files: a single SKSE plugin.
- Decals on objects and characters are left alone; only terrain decals are affected.
- Works with Community Shaders.
- Saves made before installing keep any pieces they already lost. New decals are saved whole.

## How it works

The impact code (Address Library ID 44186) finds the land bodies the decal touches in the physics world and, for each one, calls `BGSDecalManager::ApplyDecal` (ID 15203) with the quadrant mesh it reads from the body: havok property 1 points at the quadrant's `BSTriShape`, whose parent is the quadrant mesh. Each call clips the decal to that mesh.

- A detour on `ApplyDecal` (MinHook) places every terrain piece with `force` set and gives it the cell that owns the mesh.
- Land bodies of cells that come back into the loaded grid lose property 1 a few seconds after the cell enters the grid, when it is attached again. Such cells are watched for 60 seconds after each entry: a ray per quadrant finds the land body (collision layer 17), and a missing property 1 is set back to the quadrant's `BSTriShape`, under the physics world lock.
- The pieces of one decal are kept as a group. When one of them is gone from its mesh's decal list, the others are hidden. A piece is only touched while the game still holds it in such a list; the plugin never takes anything out of the game's lists.

The log is `Documents/My Games/Skyrim Special Edition/SKSE/DecalSeamFix.log`.

## Building

Requirements: Visual Studio 2022 or later (C++ desktop development), [xmake](https://xmake.io).

```
git clone --recursive https://github.com/Neutral9/DecalSeamFix.git
cd DecalSeamFix
xmake f -p windows -m releasedbg
xmake build
```

Build from PowerShell or a VS developer prompt (from Git Bash xmake may pick MinGW). `xmake install` copies the plugin into `%XSE_TES5_MODS_PATH%/DecalSeamFix` (a Mod Organizer 2 mod folder).

Layout:

- `src/DecalHook.cpp` – the detour, the land body repair and the decal groups
- `src/Plugin.cpp` – SKSE entry point and log
- `lib/commonlibsse-ng` – [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR) (submodule)

## License

[GPL-3.0](LICENSE)
