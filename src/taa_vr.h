#pragma once

// Swaps the game's temporal anti-aliasing shader for the sharper VR version
// (Cfg().taaMode: 0 = game's own, 1 = net, 2 = natural, 3 = sharp, 4 = maximum;
// the control file's taaMode=8: no anti-aliasing, test only). Call from the main
// thread before each Renderer::render, with the renderer's TemporalAa object.
void TaaVrUpdate(unsigned char* taa);

// The anti-aliasing the last TaaVrUpdate left bound (main thread): -1 the game's own
// pipeline (taaMode 0, the VR one could not be created, or no TemporalAa object),
// else the taaMode whose VR shader is bound - 1-4, 8 (raw, test), 9 (test file).
// Only 2, 3 and 4 (natural, sharp, maximum) have GLASS_FIX, the 4-neighbour mix that
// cancels a pixel checkerboard on particles and glass (particles_vr.cpp, stereoParity
// auto).
int TaaVrBoundMode();
