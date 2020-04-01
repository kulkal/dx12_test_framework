//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "Common.hlsl"

Texture2D AlbedoTex : register(t0);
Texture2D NormalTex : register(t1);
Texture2D ShadowTex : register(t2);
Texture2D VelocityTex : register(t3);
Texture2D DepthTex : register(t4);
Texture2D GIResultSHTex : register(t5);
Texture2D GIResultColorTex : register(t6);
Texture2D SpecularGITex : register(t7);
Texture2D RoughnessMetalicTex : register(t8);






SamplerState sampleWrap : register(s0);


cbuffer LightingParam : register(b0)
{
    float4 LightDirAndIntensity;
    float2 RTSize;
    float TAABlendFactor;
    float GIBufferScale;
};

struct VSInput
{
    float4 position : POSITION;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(
    VSInput input)
{
    PSInput result;

    result.position = input.position;
    result.uv = input.uv;

    return result;
}


float4 PSMain(PSInput input) : SV_TARGET
{
    float LowFreqWeight = 0.25f;
    float HiFreqWeight = 0.85f;
    float3 clrMin = 99999999.0f;
    float3 clrMax = -99999999.0f;
    float totalWeight = 0.0f;

    input.uv.y = 1 - input.uv.y;
    float2 PixelPos = input.uv * RTSize;


    float3 Albedo = AlbedoTex[PixelPos];
    float3 WorldNormal = NormalTex[PixelPos];
    float3 Shadow = ShadowTex[PixelPos];

    float2 Velocity = VelocityTex[PixelPos];

    float3 LightDir = LightDirAndIntensity.xyz;
    float LightIntensity = LightDirAndIntensity.w;
	
    float3 DiffuseLighting = dot(LightDir.xyz, WorldNormal) * LightIntensity * Albedo * Shadow;

    float2 ScreenUV = input.uv;
    SH sh_indirect;
    sh_indirect.shY = GIResultSHTex[PixelPos/GIBufferScale];
    sh_indirect.CoCg = GIResultColorTex[PixelPos/GIBufferScale].xy;

    // sh_indirect.shY = GIResultSHTex.Sample(sampleWrap, ScreenUV);
    // sh_indirect.CoCg = GIResultColorTex.Sample(sampleWrap, ScreenUV);

    float3 IndirectDiffuse = project_SH_irradiance(sh_indirect, WorldNormal) * Albedo;


    // use 0.05 if is non-metal
    // specular = mix(0.05, 1.0, metallic); 
    float Metalic = RoughnessMetalicTex[PixelPos].y;
    float Specular = lerp(0.05, 1.0, Metalic); 
    // non-metal doesnt have specular color
    // vec3 spec_color = mix(vec3(1), surf_albedo.rgb, surf_metallic) * surf_specular;
    float3 SpecularColor = lerp(1..xxxx, Albedo.xyz, Metalic) * Specular;
    // final_color = (projected_lf.rgb + high_freq.rgb) * diff_color + specular.rgb * spec_color;
    float3 IndirectSpecular = SpecularGITex[PixelPos].xyz * SpecularColor;
    return float4(DiffuseLighting + IndirectDiffuse*(1-Specular) + IndirectSpecular, 1);
}