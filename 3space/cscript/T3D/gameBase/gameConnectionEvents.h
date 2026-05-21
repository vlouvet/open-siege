#ifndef _GAMECONNECTIONEVENTS_SHIM_H_
#define _GAMECONNECTIONEVENTS_SHIM_H_
// Open Siege spec 15/02b — pulls in NetConnection so simDatablock.cpp
// can reach NetConnection::getErrorBuffer() during reloadOnLocalClient.
#include "sim/netConnection.h"
#endif
