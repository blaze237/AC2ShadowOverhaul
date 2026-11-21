/* Compiles every effect's HLSL with the system d3dcompiler_47.dll and writes its shaders_gen.h
   (bytecode arrays plus their disassembly):
       src/core/shaders/geoprep.hlsl       -> src/core/shaders_gen.h  (namespace core: the shared geometry prep)
       src/shaders/ao.hlsl                 -> src/shaders_gen.h       (ambient occlusion, contact shadows)
       src/ssgi/shaders/ssgi.hlsl          -> src/ssgi/shaders_gen.h  (namespace ssgi)
       src/vol/shaders/volumetrics.hlsl    -> src/vol/shaders_gen.h   (namespace vol)
       src/taa/shaders/taa.hlsl            -> src/taa/shaders_gen.h   (namespace taa)
   Run from the repo root after editing an HLSL file; name effects (core, ao, ssgi, vol, taa) to do only those:
       x86_64-w64-mingw32-clang -O1 -o tools/gen_shaders.exe tools/gen_shaders.c && tools/gen_shaders.exe [core ao ssgi vol taa]
   A header whose HLSL doesn't compile is deleted (restore it with git). The same compiler gives the
   same bytecode, so an unchanged HLSL file regenerates the same arrays. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Blob Blob;
typedef struct
{
	HRESULT(WINAPI* QueryInterface)(Blob*, const void*, void**);
	ULONG(WINAPI* AddRef)(Blob*);
	ULONG(WINAPI* Release)(Blob*);
	void*(WINAPI* GetBufferPointer)(Blob*);
	SIZE_T(WINAPI* GetBufferSize)(Blob*);
} BlobVtbl;
struct Blob
{
	const BlobVtbl* v;
};

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, Blob**, Blob**);
typedef HRESULT(WINAPI* PFN_Disasm)(const void*, SIZE_T, UINT, const char*, Blob**);

typedef struct
{
	const char *entry, *profile, *name;
} Shader;

typedef struct
{
	const char* id;        /* command line name */
	const char* hlsl;      /* source, from the repo root */
	const char* header;    /* output */
	const char* ns;        /* C++ namespace around the arrays, or NULL */
	const Shader* shaders;
	size_t count;
} Effect;

static const Shader kCore[] = {
	{ "PS_GeoPrep", "ps_3_0", "kGeoPrepPs" }, /* runs inside AO's / SSGI's passes, with their vertex shader */
	{ "PS_GeoPrepNormals", "ps_3_0", "kGeoPrepNormalsPs" }, /* the same with the shading normals (core/geoprep.h) */
};
static const Shader kAo[] = {
	{ "VS_Fullscreen", "vs_3_0", "kAoFullscreenVs" },
	{ "PS_GTAO", "ps_3_0", "kAoGtaoPs" },
	{ "PS_Blur", "ps_3_0", "kAoBlurPs" },
	{ "PS_BlurNarrow", "ps_3_0", "kAoBlurNarrowPs" }, /* while TAA averages the frames */
	{ "PS_Composite", "ps_3_0", "kAoCompositePs" },
	{ "PS_DepthCopy", "ps_2_0", "kDepthToColourPs" },
};
static const Shader kSsgi[] = {
	{ "VS_Fullscreen", "vs_3_0", "kGiFullscreenVs" },
	{ "PS_Radiance", "ps_3_0", "kGiRadiancePs" },
	{ "PS_Trace", "ps_3_0", "kGiTracePs" },
	{ "PS_Temporal", "ps_3_0", "kGiTemporalPs" },
	{ "PS_Blur", "ps_3_0", "kGiBlurPs" },
	{ "PS_Debug", "ps_3_0", "kGiDebugPs" },
};
static const Shader kVol[] = {
	{ "VS_Fullscreen", "vs_3_0", "kVolFullscreenVs" },
	{ "PS_FogMarch", "ps_3_0", "kVolFogMarchPs" },
	{ "PS_FogBlur", "ps_3_0", "kVolFogBlurPs" },
	{ "PS_ShaftsMask", "ps_3_0", "kVolShaftsMaskPs" },
	{ "PS_ShaftsBlur", "ps_3_0", "kVolShaftsBlurPs" },
	{ "PS_Composite", "ps_3_0", "kVolCompositePs" },
};
static const Shader kTaa[] = {
	{ "VS_Fullscreen", "vs_3_0", "kTaaFullscreenVs" },
	{ "PS_Resolve", "ps_3_0", "kTaaResolvePs" },
	{ "PS_Output", "ps_3_0", "kTaaOutputPs" },
};
#define N(a) (sizeof(a) / sizeof((a)[0]))
static const Effect kEffects[] = {
	{ "core", "src/core/shaders/geoprep.hlsl", "src/core/shaders_gen.h", "core", kCore, N(kCore) },
	{ "ao", "src/shaders/ao.hlsl", "src/shaders_gen.h", NULL, kAo, N(kAo) },
	{ "ssgi", "src/ssgi/shaders/ssgi.hlsl", "src/ssgi/shaders_gen.h", "ssgi", kSsgi, N(kSsgi) },
	{ "vol", "src/vol/shaders/volumetrics.hlsl", "src/vol/shaders_gen.h", "vol", kVol, N(kVol) },
	{ "taa", "src/taa/shaders/taa.hlsl", "src/taa/shaders_gen.h", "taa", kTaa, N(kTaa) },
};

static PFN_Compile g_compile;
static PFN_Disasm g_disasm;

/* Returns 0 on success; on a compile error the header is deleted. */
static int Generate(const Effect* e)
{
	FILE* in = fopen(e->hlsl, "rb");
	if (!in)
		return printf("cannot read %s (run from the repo root)\n", e->hlsl), 1;
	fseek(in, 0, SEEK_END);
	long size = ftell(in);
	fseek(in, 0, SEEK_SET);
	char* src = (char*)malloc(size + 1);
	if (!src || fread(src, 1, size, in) != (size_t)size)
		return fclose(in), printf("cannot read %s\n", e->hlsl), 1;
	src[size] = 0;
	fclose(in);

	FILE* f = fopen(e->header, "w");
	if (!f)
		return free(src), printf("cannot write %s\n", e->header), 1;
	printf("== %s -> %s\n", e->hlsl, e->header);
	fprintf(f, "// Generated from %s by tools/gen_shaders.c - do not edit.\n#pragma once\n#include <windows.h>\n", e->hlsl);
	if (e->ns)
		fprintf(f, "\nnamespace %s\n{\n", e->ns);
	for (size_t s = 0; s < e->count; s++)
	{
		const Shader* sh = &e->shaders[s];
		Blob *code = NULL, *err = NULL, *dis = NULL;
		const UINT flags = 1u << 15; /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */
		HRESULT hr = g_compile(src, size, e->hlsl, NULL, NULL, sh->entry, sh->profile, flags, 0, &code, &err);
		if (FAILED(hr))
		{
			printf("%s: compile failed:\n%s\n", sh->entry, err ? (const char*)err->v->GetBufferPointer(err) : "?");
			fclose(f);
			remove(e->header);
			free(src);
			return 1;
		}
		if (err)
			printf("%s: %s\n", sh->entry, (const char*)err->v->GetBufferPointer(err));
		const unsigned int* p = (const unsigned int*)code->v->GetBufferPointer(code);
		size_t n = code->v->GetBufferSize(code) / 4;
		g_disasm(p, n * 4, 0, NULL, &dis);
		const char* text = (const char*)dis->v->GetBufferPointer(dis);
		const char* slots = strstr(text, "// approximately");
		printf("%-16s %s  %s", sh->entry, sh->profile, slots ? slots : "\n");
		fprintf(f, "\n/* %s (%s)\n%s*/\n", sh->entry, sh->profile, text);
		fprintf(f, "static const DWORD %s[] = {", sh->name);
		for (size_t i = 0; i < n; i++)
			fprintf(f, "%s0x%08X,", i % 8 ? " " : "\n\t", p[i]);
		fprintf(f, "\n};\n");
	}
	if (e->ns)
		fprintf(f, "\n} // namespace %s\n", e->ns);
	fclose(f);
	free(src);
	return 0;
}

int main(int argc, char** argv)
{
	HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
	if (!m)
		return printf("d3dcompiler_47.dll not found\n"), 1;
	g_compile = (PFN_Compile)GetProcAddress(m, "D3DCompile");
	g_disasm = (PFN_Disasm)GetProcAddress(m, "D3DDisassemble");
	if (!g_compile || !g_disasm)
		return printf("d3dcompiler_47.dll has no D3DCompile / D3DDisassemble\n"), 1;
	int failed = 0;
	for (size_t k = 0; k < N(kEffects); k++)
	{
		int wanted = argc < 2;
		for (int a = 1; a < argc; a++)
			wanted |= strcmp(argv[a], kEffects[k].id) == 0;
		if (wanted)
			failed += Generate(&kEffects[k]);
	}
	if (failed)
		printf("%d header(s) not written\n", failed);
	return failed ? 1 : 0;
}
