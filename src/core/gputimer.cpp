// GPU frame times of the effects: see gputimer.h.
//
// One query set per frame in a ring: a disjoint query bracketing the frame's measurements, the
// timestamp frequency, and a begin / end timestamp pair per bracketed span (an effect may bracket
// more than one span in a frame: they add up). A set is read back with GetData(0) at a later frame's
// end; a set still in flight when its turn comes again means that frame isn't measured.

#include "core/gputimer.h"
#include "common.h"

namespace core
{

namespace
{

enum
{
	kSlots = 5,  // frames in flight
	kPairs = 12, // spans per frame
};

struct Slot
{
	IDirect3DQuery9 *disjoint = nullptr, *freq = nullptr;
	IDirect3DQuery9 *begin[kPairs] = {}, *end[kPairs] = {};
	int id[kPairs] = {};
	bool closed[kPairs] = {};
	int pairs = 0;
	bool issued = false; // waiting for its results

	void Release()
	{
		auto rel = [](IDirect3DQuery9*& q) {
			if (q)
				q->Release();
			q = nullptr;
		};
		rel(disjoint);
		rel(freq);
		for (int i = 0; i < kPairs; i++)
		{
			rel(begin[i]);
			rel(end[i]);
		}
		pairs = 0;
		issued = false;
	}
};

Slot g_slots[kSlots];
int g_next = 0;
Slot* g_open = nullptr;    // the frame being measured
bool g_skipFrame = false;  // this frame's slot was still in flight
volatile bool g_on = false;
bool g_failed = false;

// Results (read by the overlay on the same thread).
GpuTimes g_times;
DWORD g_lastRun[GT_COUNT] = {};
LARGE_INTEGER g_lastPresent = {}, g_qpf = {};

const char* const kNames[GT_COUNT] = { "AO + contact shadows", "SSGI", "Volumetrics", "TAA", "Motion blur" };

float Smooth(float old, float v, bool first) { return first ? v : old + (v - old) * 0.08f; }

bool MakeQuery(IDirect3DDevice9* dev, D3DQUERYTYPE type, IDirect3DQuery9** q)
{
	if (*q)
		return true;
	return SUCCEEDED(dev->CreateQuery(type, q)) && *q;
}

void Fail()
{
	g_failed = true;
	g_times.failed = true;
	g_open = nullptr;
	for (Slot& s : g_slots)
		s.Release();
	LogInfo("Frame times: the driver has no GPU timestamp queries; the effects' GPU times can't be shown.");
}

// Reads back every finished set (never waits).
void Poll()
{
	for (Slot& s : g_slots)
	{
		if (!s.issued)
			continue;
		BOOL disjoint = TRUE;
		UINT64 freq = 0;
		if (s.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || s.freq->GetData(&freq, sizeof(freq), 0) != S_OK)
			continue;
		double sum[GT_COUNT] = {};
		bool ran[GT_COUNT] = {}, ready = true;
		for (int i = 0; i < s.pairs && ready; i++)
		{
			if (!s.closed[i])
				continue;
			UINT64 b = 0, e = 0;
			if (s.begin[i]->GetData(&b, sizeof(b), 0) != S_OK || s.end[i]->GetData(&e, sizeof(e), 0) != S_OK)
				ready = false;
			else if (e >= b && freq)
			{
				sum[s.id[i]] += (double)(e - b) * 1000.0 / (double)freq;
				ran[s.id[i]] = true;
			}
		}
		if (!ready)
			continue;
		s.issued = false;
		if (disjoint || !freq)
			continue; // the clock changed during the frame: not usable
		const DWORD now = GetTickCount();
		double total = 0;
		for (int id = 0; id < GT_COUNT; id++)
		{
			if (!ran[id])
				continue;
			bool first = !g_lastRun[id] || now - g_lastRun[id] > 500;
			g_times.ms[id] = Smooth(g_times.ms[id], (float)sum[id], first);
			g_lastRun[id] = now ? now : 1;
			total += sum[id];
		}
		g_times.totalMs = Smooth(g_times.totalMs, (float)total, !g_times.valid);
		g_times.valid = true;
	}
}

} // namespace

const char* GpuTimer_Name(int id) { return id >= 0 && id < GT_COUNT ? kNames[id] : "?"; }

void GpuTimer_SetOn(bool on)
{
	if (on == g_on)
		return;
	g_on = on;
	// A fresh start each time the list is shown.
	bool failed = g_times.failed;
	g_times = GpuTimes();
	g_times.failed = failed;
	for (DWORD& t : g_lastRun)
		t = 0;
	g_lastPresent.QuadPart = 0;
}

bool GpuTimer_On() { return g_on; }

void GpuTimer_Begin(IDirect3DDevice9* dev, int id)
{
	if (!g_on || g_failed || g_skipFrame || !dev || id < 0 || id >= GT_COUNT)
		return;
	if (!g_open)
	{
		Slot& s = g_slots[g_next];
		if (s.issued)
		{
			Poll();
			if (s.issued)
			{
				g_skipFrame = true; // the GPU is that far behind: this frame isn't measured
				return;
			}
		}
		if (!MakeQuery(dev, D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint) || !MakeQuery(dev, D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq))
		{
			Fail();
			return;
		}
		s.pairs = 0;
		s.disjoint->Issue(D3DISSUE_BEGIN);
		g_open = &s;
	}
	Slot& s = *g_open;
	if (s.pairs >= kPairs)
		return;
	const int i = s.pairs;
	if (!MakeQuery(dev, D3DQUERYTYPE_TIMESTAMP, &s.begin[i]) || !MakeQuery(dev, D3DQUERYTYPE_TIMESTAMP, &s.end[i]))
	{
		Fail();
		return;
	}
	s.id[i] = id;
	s.closed[i] = false;
	s.pairs++;
	s.begin[i]->Issue(D3DISSUE_END);
}

void GpuTimer_End(int id)
{
	if (!g_open)
		return;
	Slot& s = *g_open;
	for (int i = s.pairs - 1; i >= 0; i--)
		if (s.id[i] == id && !s.closed[i])
		{
			s.end[i]->Issue(D3DISSUE_END);
			s.closed[i] = true;
			return;
		}
}

void GpuTimer_EndFrame(IDirect3DDevice9* dev)
{
	(void)dev;
	g_skipFrame = false;
	if (!g_on)
		return;
	if (g_open)
	{
		Slot& s = *g_open;
		s.freq->Issue(D3DISSUE_END);
		s.disjoint->Issue(D3DISSUE_END);
		s.issued = true;
		g_open = nullptr;
		g_next = (g_next + 1) % kSlots;
	}
	Poll();

	// The whole frame, for scale.
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (!g_qpf.QuadPart)
		QueryPerformanceFrequency(&g_qpf);
	if (g_lastPresent.QuadPart && g_qpf.QuadPart)
	{
		float ms = (float)((double)(now.QuadPart - g_lastPresent.QuadPart) * 1000.0 / (double)g_qpf.QuadPart);
		if (ms < 1000.0f) // not across a pause / alt-tab
			g_times.frameMs = Smooth(g_times.frameMs, ms, g_times.frameMs <= 0);
	}
	g_lastPresent = now;
}

void GpuTimer_OnLostDevice()
{
	g_open = nullptr;
	g_skipFrame = false;
	for (Slot& s : g_slots)
		s.Release();
}

GpuTimes GpuTimer_Get()
{
	GpuTimes t = g_times;
	const DWORD now = GetTickCount();
	for (int id = 0; id < GT_COUNT; id++)
		t.active[id] = g_lastRun[id] && now - g_lastRun[id] < 500;
	return t;
}

} // namespace core
