/*
 * Online play left out (make NET=0, and the Dreamcast build): what the game, the renderer and the settings window
 * ask of port/src/gs/net.c, net_chat.c, net_match.c and state.c, answered as a copy that is never in a session.
 * The lobby fails at once with the reason (state -3), so the online screen says so instead of waiting.
 */
#include <string.h>
#include "../gs/net_chat.h"

volatile int gPortNetWindowClose;
int gPortResim; /* gs_internal.h: never, without rollback */

/* gs/net.c: the game's questions */
int Port_NetActive(void) { return 0; }
int Port_NetSession(void) { return 0; }
int Port_NetMe(void) { return 0; }
int Port_NetView(void) { return -1; }
int Port_HudSwap(void) { return 0; }
int Port_NetWarp(void) { return 0; }
int Port_NetResim(void) { return 0; }
int Port_NetFixedStart(void) { return 0; }
int Port_NetVoiceAlt(void) { return 0; }
int Port_NetSideOptions(int *camera, int *shake) { (void)camera; (void)shake; return 0; }
int Port_NetSpeedMilli(void) { return 1000; }
int Port_NetSpeedStandard(void) { return 114; }
unsigned Port_NetSeed(void) { return 0; }
const char *Port_NetName(int player) { (void)player; return ""; }
void Port_NetRules(int *type, int *dp, int *time) { *type = 0; *dp = 0; *time = 3; }
void Port_NetInput(int player, unsigned char *data) {
    static const unsigned char idle[6] = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    (void)player;
    memcpy(data, idle, sizeof(idle));
}
int Port_NetStats(int *out) { (void)out; return 0; }
void Port_NetBeginTick(unsigned tick) { (void)tick; }
void Port_NetArrived(void) {}
void Port_NetLeave(void) {}
void Port_NetOptions(int rollback, int delay) { (void)rollback; (void)delay; }
void Port_NetRulesSet(int type, int dp, int time) { (void)type; (void)dp; (void)time; }
void Port_NetSpeedSet(int percent) { (void)percent; }
void Port_NetNameSet(const char *name) { (void)name; }
void Port_TestSlow(int what, int amount) { (void)what; (void)amount; }

/* the lobby (the settings window's online screen) */
void Port_LobbyAuto(void) {}
int Port_LobbyStart(int host, const char *address, int port) { (void)host; (void)address; (void)port; return 0; }
int Port_LobbyStartCode(int host, const char *code) { (void)host; (void)code; return 0; }
int Port_LobbyStartQuick(int queue) { (void)queue; return 0; }
int Port_LobbyPoll(void) { return 0; }
void Port_LobbyCancel(void) {}
void Port_LobbyLaunch(void) {}
int Port_LobbyOther(void) { return 0; }
int Port_LobbyRelayed(void) { return 0; }
void Port_LobbyQueuesAsk(void) {}
void Port_LobbyHelloAsk(void) {}
int Port_LobbyQueueWaiting(int queue) { (void)queue; return -1; }
int Port_LobbyRelease(int *min, int *latest) { *min = *latest = 0; return 0; }
const char *Port_LobbyCode(void) { return ""; }
const char *Port_LobbyError(void) { return "online play is not in this build"; }

/* net_match.c: the check for a newer release */
void Match_UpdateAsk(void) {}
int Match_UpdateLatest(void) { return 0; }

/* net_chat.c */
unsigned PortChat_Count(void) { return 0; }
int PortChat_Line(unsigned index, PortChatLine *out) { (void)index; (void)out; return 0; }
int PortChat_Say(const char *text) { (void)text; return 0; }
int PortChat_Enabled(void) { return 0; }
void PortChat_EnabledSet(int on) { (void)on; }
int PortChat_Muted(void) { return 0; }
void PortChat_MutedSet(int on) { (void)on; }

/* state.c: the whole-game snapshots behind rollback and sessions */
int Port_RollCan(void) { return 0; }
void Port_SessionPoll(void) {}
void Port_StateTouch(void *p, unsigned long n) { (void)p; (void)n; }
void Port_StateLog(unsigned vblank, const void *fight, unsigned fightSize) { (void)vblank; (void)fight; (void)fightSize; }
void Port_SyncTest(unsigned vblank) { (void)vblank; }
