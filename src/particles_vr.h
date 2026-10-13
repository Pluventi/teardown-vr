#pragma once
#include <cstdint>

// [Affichage] stereoParticles: rain, smoke, fire, the extinguisher and every particle drawn with the same dither and the
// same front/back lighting in both eyes, and the noise of the ambient occlusion and sun rays taken at the same 3D point
// in both eyes (particles_vr.cpp, shaders/gbufferparticles_vr.hlsl, shaders/ambientdirectionallight_vr.hlsl).
//
// 1. From d3d_hooks.cpp at every DXGI factory creation, until it is done: the game's code is decrypted by then and its
//    renderer has not built its shaders yet. Hooks the engine's pipeline creation (a VR twin of each particle and
//    lighting pipeline, the game's own state with our pixel shader) and the upload of the scene constants.
void ParticlesVrEarly();
// 2. Main thread, Hook_Render, before Renderer::render and after TaaVrUpdate: eye = 0 a stereo pair's frame A (left
//    eye), 1 its frame B (right eye), -1 not a stereo pair; frame = the engine frame number. Binds the VR or the game's
//    particle and lighting pipelines (the setting, live) - for the particles the variant of the right eye's front/back
//    checkerboard ([Affichage] stereoParity, the control file's, or auto from the anti-aliasing TaaVrUpdate bound) - and
//    tells the scene constants which eye this render draws.
void ParticlesVrBeginRender(unsigned char* renderer, int eye, uint64_t frame);
// 3. Main thread, Hook_Render, after Renderer::render.
void ParticlesVrEndRender();
// For the settings panel: 0 = not available (game version not recognised, or its shaders were made before the
// hooks), 1 = available (the game's pipelines bound), 2 = VR pipelines are bound.
int ParticlesVrStatus();
// false in a build without the game-derived shaders (CMake TDVR_GAME_SHADERS=OFF): the stereo particles and the VR
// anti-aliasing modes do not exist in it (the settings panel says so).
bool GameShadersBuilt();
