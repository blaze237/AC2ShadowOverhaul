/* Compiles src/shaders/ssgi.hlsl with the system d3dcompiler_47.dll and writes src/shaders_gen.h
   (bytecode arrays plus their disassembly). Run from the repo root after editing the HLSL:
       x86_64-w64-mingw32-clang -O1 -o tools/gen_shaders.exe tools/gen_shaders.c && tools/gen_shaders.exe */
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

static const struct
{
	const char *entry, *profile, *name;
} kShaders[] = {
	{ "VS_Fullscreen", "vs_3_0", "kGiFullscreenVs" },
	{ "PS_Prep", "ps_3_0", "kGiPrepPs" },
	{ "PS_Trace", "ps_3_0", "kGiTracePs" },
	{ "PS_Temporal", "ps_3_0", "kGiTemporalPs" },
	{ "PS_Blur", "ps_3_0", "kGiBlurPs" },
	{ "PS_Debug", "ps_3_0", "kGiDebugPs" },
};

int main(void)
{
	const char* srcPath = "src/shaders/ssgi.hlsl";
	FILE* in = fopen(srcPath, "rb");
	if (!in)
		return printf("cannot read %s (run from the repo root)\n", srcPath), 1;
	fseek(in, 0, SEEK_END);
	long size = ftell(in);
	fseek(in, 0, SEEK_SET);
	char* src = (char*)malloc(size + 1);
	fread(src, 1, size, in);
	src[size] = 0;
	fclose(in);

	HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
	if (!m)
		return printf("d3dcompiler_47.dll not found\n"), 1;
	PFN_Compile compile = (PFN_Compile)GetProcAddress(m, "D3DCompile");
	PFN_Disasm disasm = (PFN_Disasm)GetProcAddress(m, "D3DDisassemble");

	FILE* f = fopen("src/shaders_gen.h", "w");
	if (!f)
		return printf("cannot write src/shaders_gen.h\n"), 1;
	fprintf(f, "// Generated from src/shaders/ssgi.hlsl by tools/gen_shaders.c - do not edit.\n#pragma once\n#include <windows.h>\n");
	for (size_t s = 0; s < sizeof(kShaders) / sizeof(kShaders[0]); s++)
	{
		Blob *code = NULL, *err = NULL, *dis = NULL;
		const UINT flags = 1u << 15; /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */
		HRESULT hr = compile(src, size, srcPath, NULL, NULL, kShaders[s].entry, kShaders[s].profile, flags, 0, &code, &err);
		if (FAILED(hr))
		{
			printf("%s: compile failed:\n%s\n", kShaders[s].entry, err ? (const char*)err->v->GetBufferPointer(err) : "?");
			fclose(f);
			remove("src/shaders_gen.h");
			return 1;
		}
		if (err)
			printf("%s: %s\n", kShaders[s].entry, (const char*)err->v->GetBufferPointer(err));
		const unsigned int* p = (const unsigned int*)code->v->GetBufferPointer(code);
		size_t n = code->v->GetBufferSize(code) / 4;
		disasm(p, n * 4, 0, NULL, &dis);
		const char* text = (const char*)dis->v->GetBufferPointer(dis);
		const char* slots = strstr(text, "// approximately");
		printf("%-16s %s  %s", kShaders[s].entry, kShaders[s].profile, slots ? slots : "\n");
		fprintf(f, "\n/* %s (%s)\n%s*/\n", kShaders[s].entry, kShaders[s].profile, text);
		fprintf(f, "static const DWORD %s[] = {", kShaders[s].name);
		for (size_t i = 0; i < n; i++)
			fprintf(f, "%s0x%08X,", i % 8 ? " " : "\n\t", p[i]);
		fprintf(f, "\n};\n");
	}
	fclose(f);
	return 0;
}
