#include <StdInc.h>

#include <Hooking.h>
#include <NetLibrary.h>

#include <CoreNetworking.h>

static NetLibrary* g_netLibrary;

// shared relay functions (from early rev. gta:net:five; do update!)
#include <ws2tcpip.h>

static SOCKET g_gameSocket;
static int lastReceivedFrom;

static ULONG g_pendSendVar;

int __stdcall CfxRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen)
{
	static char buffer[65536];
	size_t length;
	uint16_t netID;

	if (s == g_gameSocket)
	{
		if (g_netLibrary->DequeueRoutedPacket(buffer, &length, &netID))
		{
			memcpy(buf, buffer, fwMin((size_t)len, length));

			sockaddr_in* outFrom = (sockaddr_in*)from;
			memset(outFrom, 0, sizeof(sockaddr_in));

			outFrom->sin_family = AF_INET;
			outFrom->sin_addr.s_addr = ntohl((netID ^ 0xFEED) | 0xC0A80000);
			outFrom->sin_port = htons(6672);

			char addr[60];
			inet_ntop(AF_INET, &outFrom->sin_addr.s_addr, addr, sizeof(addr));

			*fromlen = sizeof(sockaddr_in);

			lastReceivedFrom = netID;

			if (CoreIsDebuggerPresent() && false)
			{
				trace("CfxRecvFrom (from %i %s) %i bytes on %p\n", netID, addr, length, (void*)s);
			}

			return fwMin((size_t)len, length);
		}
		else
		{
			WSASetLastError(WSAEWOULDBLOCK);
			return SOCKET_ERROR;
		}
	}

	return recvfrom(s, buf, len, flags, from, fromlen);
}

int __stdcall CfxSendTo(SOCKET s, char* buf, int len, int flags, sockaddr* to, int tolen)
{
	sockaddr_in* toIn = (sockaddr_in*)to;

	if (s == g_gameSocket)
	{
		if (toIn->sin_addr.S_un.S_un_b.s_b1 == 0xC0 && toIn->sin_addr.S_un.S_un_b.s_b2 == 0xA8)
		{
			g_pendSendVar = 0;

			if (CoreIsDebuggerPresent() && false)
			{
				trace("CfxSendTo (to internal address %i) %i b (from thread 0x%x)\n", (htonl(toIn->sin_addr.s_addr) & 0xFFFF) ^ 0xFEED, len, GetCurrentThreadId());
			}
		}
		else
		{
			char publicAddr[256];
			inet_ntop(AF_INET, &toIn->sin_addr.s_addr, publicAddr, sizeof(publicAddr));

			if (toIn->sin_addr.s_addr == 0xFFFFFFFF)
			{
				return len;
			}

			trace("CfxSendTo (to %s) %i b\n", publicAddr, len);
		}

		g_netLibrary->RoutePacket(buf, len, (uint16_t)((htonl(toIn->sin_addr.s_addr)) & 0xFFFF) ^ 0xFEED);

		return len;
	}

	return sendto(s, buf, len, flags, to, tolen);
}

int __stdcall CfxBind(SOCKET s, sockaddr* addr, int addrlen)
{
	sockaddr_in* addrIn = (sockaddr_in*)addr;

	trace("binder on %i is %p\n", htons(addrIn->sin_port), s);

	if (htons(addrIn->sin_port) == 6672)
	{
		if (wcsstr(GetCommandLine(), L"cl2"))
		{
			addrIn->sin_port = htons(6673);
		}

		g_gameSocket = s;
	}

	return bind(s, addr, addrlen);
}

int __stdcall CfxGetSockName(SOCKET s, struct sockaddr* name, int* namelen)
{
	int retval = getsockname(s, name, namelen);

	sockaddr_in* addrIn = (sockaddr_in*)name;

	if (s == g_gameSocket && wcsstr(GetCommandLine(), L"cl2"))
	{
		addrIn->sin_port = htons(6672);
	}

	return retval;
}

int __stdcall CfxSelect(_In_ int nfds, _Inout_opt_ fd_set FAR* readfds, _Inout_opt_ fd_set FAR* writefds, _Inout_opt_ fd_set FAR* exceptfds, _In_opt_ const struct timeval FAR* timeout)
{
	bool shouldAddSocket = false;

	if (readfds)
	{
		for (int i = 0; i < readfds->fd_count; i++)
		{
			if (readfds->fd_array[i] == g_gameSocket)
			{
				memmove(&readfds->fd_array[i + 1], &readfds->fd_array[i], readfds->fd_count - i - 1);
				readfds->fd_count -= 1;
				nfds--;

				if (g_netLibrary->WaitForRoutedPacket((timeout) ? ((timeout->tv_sec * 1000) + (timeout->tv_usec / 1000)) : INFINITE))
				{
					shouldAddSocket = true;
				}
			}
		}
	}

	//FD_ZERO(readfds);

	if (nfds > 0)
	{
		nfds = select(nfds, readfds, writefds, exceptfds, timeout);
	}

	if (shouldAddSocket)
	{
		FD_SET(g_gameSocket, readfds);

		nfds += 1;
	}

	return nfds;
}

#include <CoreConsole.h>

#include <ICoreGameInit.h>
#include <nutsnbolts.h>

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

static hook::cdecl_stub<bool()> isNetworkHost([]()
{
	return hook::get_pattern("33 DB 38 1D ? ? ? ? 75 1B 38 1D", -6);
});

static HookFunction initFunction([]()
{
	g_netLibrary = NetLibrary::Create();

	g_netLibrary->OnBuildMessage.Connect([](const std::function<void(uint32_t, const char*, int)>& writeReliable)
	{
		static bool lastHostState;

		// hostie
		bool isHost = isNetworkHost();
		if (isHost != lastHostState)
		{
			if (isHost)
			{
				auto base = g_netLibrary->GetServerBase();
				writeReliable(0xB3EA30DE, (char*)&base, sizeof(base));
			}

			lastHostState = isHost;
		}

		static uint32_t lastHostSend = timeGetTime();

		if ((timeGetTime() - lastHostSend) > 1500)
		{
			lastHostState = false;
			lastHostSend = timeGetTime();
		}
	});

	OnGameFrame.Connect([]()
	{
		g_netLibrary->RunFrame();
	});

	//g_netLibrary->SetBase(GetTickCount());

	// set base to the ROS ID as that's the default gamer handle value
	// this needs patching, otherwise rlJoinSessionTask::Configure will complain that the alleged session host
	// is not in the list of gamers in the session
	auto hModule = GetModuleHandleW(L"ros-patches-rdr3.dll");
	
	if (hModule)
	{
		auto hProc = (uint64_t(*)())GetProcAddress(hModule, "GetAccountID");
		g_netLibrary->SetBase(static_cast<uint32_t>(hProc()));
	}
});

static hook::cdecl_stub<void(int, void*, void*)> joinOrHost([]()
{
	return hook::get_pattern("48 8D 55 30 49 83 60 10 00 44 8B F1", -32);
});

// 
static bool GetLocalPeerAddress(int localPlayerIdx, netPeerAddress* out)
{
	memset(out, 0, sizeof(*out));

	out->peerId.val = g_netLibrary->GetServerBase() | ((uint64_t)g_netLibrary->GetServerBase() << 32);
	*(uint64_t*)&out->gamerHandle.handle[0] = g_netLibrary->GetServerBase();
	*(uint8_t*)&out->gamerHandle.handle[10] = 3;
	out->localAddr.ip.addr = (g_netLibrary->GetServerNetID() ^ 0xFEED) | 0xc0a80000;
	out->localAddr.port = 6672;
	out->relayAddr.ip.addr = 0xFFFFFFFF;
	out->relayAddr.port = 0;
	out->publicAddr.ip.addr = 0x7F000001;
	out->publicAddr.port = 0;

	for (int i = 0; i < sizeof(out->peerKey) / sizeof(uint32_t); i++)
	{
		*((uint32_t*)out->peerKey + i) = g_netLibrary->GetServerBase();
	}

	out->hasPeerKey = true;

	static_assert(offsetof(netPeerAddress, hasPeerKey) == 56, "invalid");

	for (int i = 0; i < 4; i++)
	{
		out->unk.unks[i].addr.ip.addr = 0xFFFFFFFF;
	}

	return true;
}

static bool GetGamerHandle(int localPlayerIdx, rlGamerHandle* out)
{
	memset(out->handle, 0, sizeof(out->handle));
	*(uint64_t*)&out->handle[0] = g_netLibrary->GetServerBase();
	*(uint8_t*)&out->handle[10] = 3;

	return true;
}

static bool GetLocalPeerId(netPeerId* id)
{
	id->val = g_netLibrary->GetServerBase() | ((uint64_t)g_netLibrary->GetServerBase() << 32);

	return true;
}

static bool GetPublicIpAddress(netIpAddress* out)
{
	out->addr = (g_netLibrary->GetServerNetID() ^ 0xFEED) | 0xc0a80000;

	return true;
}

struct RelayAddress
{
	netSocketAddress addr1;
	netSocketAddress addr2;
	int unk;
	int unk2;
	uint8_t type;
};

static RelayAddress gRelayAddress;

static RelayAddress* GetMyRelayAddress()
{
	gRelayAddress.addr1.ip.addr = (g_netLibrary->GetServerNetID() ^ 0xFEED) | 0xc0a80000;
	gRelayAddress.addr1.port = 6672;

	gRelayAddress.addr2.ip.addr = (g_netLibrary->GetServerNetID() ^ 0xFEED) | 0xc0a80000;
	gRelayAddress.addr2.port = 6672;

	gRelayAddress.type = 0;

	return &gRelayAddress;
}

struct P2PCryptKey
{
	uint8_t key[32];
	bool inited;
};

static bool InitP2PCryptKey(P2PCryptKey* struc)
{
	for (int i = 0; i < sizeof(struc->key) / sizeof(uint32_t); i++)
	{
		*((uint32_t*)struc->key + i) = g_netLibrary->GetServerBase();
	}

	struc->inited = true;

	return true;
}

static bool ZeroUUID(uint64_t* uuid)
{
	*uuid = g_netLibrary->GetServerBase();
	return true;
}

static bool(*origSendGamer)(char* a1, uint32_t pidx, uint32_t a3, void* null, void* msg, void* a6, void* a7);

static void* curGamer;
static uint32_t playerCountOffset;
static uint32_t playerListOffset;
static uint32_t backwardsOffset;

static bool SendGamerToMultiple(char* a1, uint32_t pidx, uint32_t a3, void* null, void* msg, void* a6, void* a7)
{
	char* session = (a1 - backwardsOffset);
	int count = *(int*)(session + playerCountOffset);
	int** list = (int**)(session + playerListOffset);

	for (int i = 0; i < count; i++)
	{
		auto p = list[i];

		/*if (p == curGamer)
		{
			continue;
		}*/

		origSendGamer(a1, *p, a3, null, msg, a6, a7);
	}

	return true;
}

static uint8_t* seamlessOff;

static void SetSeamlessOn(bool)
{
	*seamlessOff = 1;
}

static bool ReadSession(void* self, void* parTree, rlSessionInfo* session)
{
	if (g_netLibrary->GetHostNetID() == 0xFFFF || g_netLibrary->GetHostNetID() == g_netLibrary->GetServerNetID())
	{
		return false;
	}

	session->sessionToken.token = g_netLibrary->GetHostBase();

	auto out = &session->peerAddress;
	memset(out, 0, sizeof(*out));

	out->peerId.val = g_netLibrary->GetHostBase() | ((uint64_t)g_netLibrary->GetHostBase() << 32);
	*(uint64_t*)&out->gamerHandle.handle[0] = g_netLibrary->GetHostBase();
	*(uint8_t*)&out->gamerHandle.handle[10] = 3;
	out->localAddr.ip.addr = (g_netLibrary->GetHostNetID() ^ 0xFEED) | 0xc0a80000;
	out->localAddr.port = 6672;
	out->relayAddr.ip.addr = 0xFFFFFFFF;
	out->relayAddr.port = 0;
	out->publicAddr.ip.addr = 0x7F000001;
	out->publicAddr.port = 0;

	for (int i = 0; i < sizeof(out->peerKey) / sizeof(uint32_t); i++)
	{
		*((uint32_t*)out->peerKey + i) = g_netLibrary->GetHostBase();
	}

	out->hasPeerKey = true;

	for (int i = 0; i < 4; i++)
	{
		out->unk.unks[i].addr.ip.addr = 0xFFFFFFFF;
	}

	return true;
}

static void DumpHex(const void* data, size_t size) {
	char ascii[17];
	size_t i, j;
	ascii[16] = '\0';
	for (i = 0; i < size; ++i) {
		trace("%02X ", ((unsigned char*)data)[i]);
		if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
			ascii[i % 16] = ((unsigned char*)data)[i];
		}
		else {
			ascii[i % 16] = '.';
		}
		if ((i + 1) % 8 == 0 || i + 1 == size) {
			trace(" ");
			if ((i + 1) % 16 == 0) {
				trace("|  %s \n", ascii);
			}
			else if (i + 1 == size) {
				ascii[(i + 1) % 16] = '\0';
				if ((i + 1) % 16 <= 8) {
					trace(" ");
				}
				for (j = (i + 1) % 16; j < 16; ++j) {
					trace("   ");
				}
				trace("|  %s \n", ascii);
			}
		}
	}
}

static bool ParseAddGamer(void* a1, void* a2, void* gamer)
{
	auto rv = ((bool(*)(void*, void*, void*))0x1426A77BC)(a1, a2, gamer);

	if (rv)
	{
		trace("--- ADD GAMER CMD ---\n");
		DumpHex(gamer, 384);
	}

	return rv;
}

static void LogStubLog1(void* stub, const char* type, const char* format, ...)
{
	if (type && format)
	{
		char buffer[4096];
		va_list ap;
		va_start(ap, format);
		vsnprintf(buffer, 4096, format, ap);
		va_end(ap);

		trace("%s: %s\n", type, buffer);
	}
}

#include <MinHook.h>

static int(*of1)(int*, void*);

static int f1(int* a, void* b)
{
	auto rv = of1(a, b);

	if (CoreIsDebuggerPresent() && false)
	{
		trace("RECV PKT %d\n", *a);
	}

	return rv;
}

static int(*of2)(int, void*);

static int f2(int a, void* b)
{
	auto rv = of2(a, b);

	if (CoreIsDebuggerPresent() && false)
	{
		trace("SEND PKT %d\n", a);
	}

	return rv;
}

static int Return1()
{
	return 1;
}

static void* rlPresence__m_GamerPresences;

static hook::cdecl_stub<void(void*)> _rlPresence_GamerPresence_Clear([]()
{
	return hook::get_call(hook::get_pattern("48 89 5D 38 48 89 5D 40 48 89 5D 48 E8", 12));
});

static hook::cdecl_stub<void(int)> _rlPresence_refreshSigninState([]()
{
	return hook::get_pattern("48 8D 54 24 20 48 69 F8 30 01 00 00 48 8D 05", -0x35);
});

static hook::cdecl_stub<void(int)> _rlPresence_refreshNetworkStatus([]()
{
	return hook::get_pattern("45 33 FF 8B DE EB 0F 48 8D", -0x7D);
});

static HookFunction hookFunction([]()
{
	//MH_Initialize();
	//MH_CreateHook((void*)0x1422306FC, f2, (void**)&of2);
	//MH_CreateHook((void*)0x1422304F8, f1, (void**)&of1);
	//MH_EnableHook(MH_ALL_HOOKS);

	hook::iat("ws2_32.dll", CfxSendTo, 20);
	hook::iat("ws2_32.dll", CfxRecvFrom, 17);
	hook::iat("ws2_32.dll", CfxBind, 2);
	hook::iat("ws2_32.dll", CfxSelect, 18);
	hook::iat("ws2_32.dll", CfxGetSockName, 6);

	//hook::jump(0x1406B50E8, LogStubLog1);

	{
		auto getLocalPeerAddress = hook::get_pattern<char>("48 8B D0 80 78 18 02 75 1D", -0x32);
		hook::jump(getLocalPeerAddress, GetLocalPeerAddress);
		hook::jump(hook::get_call(getLocalPeerAddress + 0x28), GetLocalPeerId);
		//hook::jump(hook::get_call(getLocalPeerAddress + 0x2D), GetMyRelayAddress);
		//hook::jump(hook::get_call(getLocalPeerAddress + 0x62), GetPublicIpAddress);
		hook::jump(hook::get_call(getLocalPeerAddress + 0xE1), GetGamerHandle);
		hook::jump(hook::get_call(getLocalPeerAddress + 0x102), InitP2PCryptKey);
		//hook::call(0x14266F66C, InitP2PCryptKey);
		//hook::call(0x14267B5A0, InitP2PCryptKey);
	}

	//
	//hook::call(0x1426E100B, ParseAddGamer);

	// all uwuids be 2
	hook::call(hook::get_pattern("B9 03 00 00 00 B8 01 00 00 00 87 83", -85), ZeroUUID);

	// get session for find result
	// 1207.58
	hook::jump(hook::get_pattern("48 85 C0 74 31 80 38 00 74 2C 45", -0x1B), ReadSession);

	seamlessOff = hook::get_address<uint8_t*>(hook::get_pattern("33 DB 38 1D ? ? ? ? 75 1B 38 1D", 4));

	// skip seamless host for is-host call
	//hook::put<uint8_t>(hook::get_pattern("75 1B 38 1D ? ? ? ? 74 36"), 0xEB);

	rlPresence__m_GamerPresences = hook::get_address<void*>(hook::get_pattern("48 8D 54 24 20 48 69 F8 30 01 00 00 48 8D 05", 0x44 - 0x35));

	static bool tryHost = true;

	OnMainGameFrame.Connect([]()
	{
		if (!Instance<ICoreGameInit>::Get()->GetGameLoaded())
		{
			return;
		}

		if (tryHost)
		{
			// update presence
			_rlPresence_GamerPresence_Clear(rlPresence__m_GamerPresences);
			_rlPresence_refreshSigninState(0);
			_rlPresence_refreshNetworkStatus(0);

			static char outBuf[48];
			joinOrHost(0, nullptr, outBuf);

			tryHost = false;
		}
	});

	static ConsoleCommand hhh("hhh", []()
	{
		tryHost = true;
	});

	// rlSession::InformPeersOfJoiner bugfix: reintroduce loop (as in, remove break; statement)
	// (by handling the _called function_ and adding the loop in a wrapper there)
	{
		auto location = hook::get_pattern<char>("4C 63 83 ? ? 00 00 4D 85 C0 7E 4F 33");

		playerCountOffset = *(uint32_t*)(location + 3);
		playerListOffset = *(uint32_t*)(location + 14 + 3);
		backwardsOffset = *(uint32_t*)(location + 62 + 3);

		hook::set_call(&origSendGamer, location + 86);

		static struct : jitasm::Frontend
		{
			void InternalMain() override
			{
				mov(rax, (uintptr_t)&curGamer);
				mov(qword_ptr[rax], rsi);

				mov(rax, (uintptr_t)SendGamerToMultiple);
				jmp(rax);
			}
		} stub;

		hook::call(location + 86, stub.GetCode());
	}

	// test: don't allow setting odd seamless mode
	hook::jump(hook::get_call(hook::get_pattern("B1 01 E8 ? ? ? ? 80 3D", 2)), SetSeamlessOn);

	// always not seamless
	hook::jump(hook::get_call(hook::get_pattern("84 C0 0F 84 2C 01 00 00 E8", 8)), Return1);
});