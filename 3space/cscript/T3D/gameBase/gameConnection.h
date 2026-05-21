#ifndef _GAMECONNECTION_SHIM_H_
#define _GAMECONNECTION_SHIM_H_
// Open Siege spec 15/02b — stub for T3D/gameBase/gameConnection.h.
// SimDataBlock::reloadOnLocalClient() uses GameConnection::getLocalClient
// Connection() to look up the active client. In a script-VM build with no
// netcode, there's never a local client. The stub returns nullptr so the
// reload call short-circuits cleanly.
class GameConnection {
public:
    static GameConnection* getLocalClientConnection() { return nullptr; }
};
#endif
