// Based on https://github.com/KhronosGroup/glTF-Sample-Renderer shaders (ibl.glsl)

// Number of mip levels above zero in the GGX prefiltered environment map.
// Must match IBLEnvironment::SPECULAR_MIPS - 1 (engine side).
const float ENV_GGX_LODS = 7.0;

// Analytic approximation of the GGX environment BRDF LUT (u_GGXLUT in the
// glTF-Sample-Renderer). From Karis "Physically Based Shading on Mobile"
// (Lazarov's approximation), returns the scale (x) and bias (y) applied to F0.
vec2 envBRDFApprox(float NdotV, float roughness){
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

#ifdef USE_IBL
    // Rotate an environment lookup direction to match the sky rotation
    vec3 rotateEnv(vec3 dir){
        float angle = lighting.envColor.w;
        float c = cos(angle);
        float s = sin(angle);
        return vec3(c * dir.x - s * dir.z, dir.y, s * dir.x + c * dir.z);
    }

    // Cosine-convolved irradiance (already divided by PI on generation)
    vec3 getDiffuseLight(vec3 n){
        vec3 color = sRGBToLinear(texture(samplerCube(u_lambertianEnvTexture, u_lambertianEnv_smp), rotateEnv(n))).rgb;
        return color * lighting.envColor.rgb;
    }

    // GGX prefiltered radiance, roughness selects the mip level
    vec3 getSpecularLight(vec3 reflection, float lod){
        vec3 color = sRGBToLinear(textureLod(samplerCube(u_GGXEnvTexture, u_GGXEnv_smp), rotateEnv(reflection), lod)).rgb;
        return color * lighting.envColor.rgb;
    }
#endif

// Specular response to an environment, including multiple scattering energy
// compensation from Fdez-Aguera "A Multiple-Scattering Microfacet Model for
// Real-Time Image Based Lighting"
vec3 envRadianceGGX(vec3 specularLight, float NdotV, float roughness, vec3 F0){
    vec2 f_ab = envBRDFApprox(NdotV, roughness);
    vec3 Fr = max(vec3(1.0 - roughness), F0) - F0;
    vec3 k_S = F0 + Fr * pow(1.0 - NdotV, 5.0);
    vec3 FssEss = k_S * f_ab.x + f_ab.y;
    return specularLight * FssEss;
}

// Diffuse response to an environment irradiance (Fdez-Aguera)
vec3 envRadianceLambertian(vec3 irradiance, float NdotV, float roughness, vec3 diffuseColor, vec3 F0){
    vec2 f_ab = envBRDFApprox(NdotV, roughness);
    vec3 Fr = max(vec3(1.0 - roughness), F0) - F0;
    vec3 k_S = F0 + Fr * pow(1.0 - NdotV, 5.0);
    vec3 FssEss = k_S * f_ab.x + f_ab.y;

    float Ems = (1.0 - (f_ab.x + f_ab.y));
    vec3 F_avg = (F0 + (1.0 - F0) / 21.0);
    vec3 FmsEms = Ems * FssEss * F_avg / (1.0 - F_avg * Ems);
    vec3 k_D = diffuseColor * (1.0 - FssEss + FmsEms);

    return (FmsEms + k_D) * irradiance;
}

#ifdef USE_IBL
    vec3 getIBLRadianceGGX(vec3 n, vec3 v, float roughness, vec3 F0){
        float NdotV = clampedDot(n, v);
        float lod = roughness * ENV_GGX_LODS;
        vec3 reflection = normalize(reflect(-v, n));
        vec3 specularLight = getSpecularLight(reflection, lod);
        return envRadianceGGX(specularLight, NdotV, roughness, F0);
    }

    vec3 getIBLRadianceLambertian(vec3 n, vec3 v, float roughness, vec3 diffuseColor, vec3 F0){
        float NdotV = clampedDot(n, v);
        vec3 irradiance = getDiffuseLight(n);
        return envRadianceLambertian(irradiance, NdotV, roughness, diffuseColor, F0);
    }
#endif
