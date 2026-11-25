#pragma once
#include <d3d11.h>
#include "LcXR.h"
namespace lcxr {
ID3D11ShaderResourceView* DrawMenu(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool srgb, void (*log)(const char*, ...), MenuSnapshot* outPlacement);
ID3D11Texture2D* MenuTexture();
float MenuAspect();  // height / width
int MenuPixelsW();
int MenuPixelsH();
}
