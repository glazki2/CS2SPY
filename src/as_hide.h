#ifndef _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <functional>
#include <string>
#include <vector>
#include "const.h"
#include "bitvec.h"
#include "variant.h"
#include "vector.h"
#include "igameevents.h"
#include "checktransmitinfo.h"
#include <entity2/entitysystem.h>
#include <KeyValues.h>
#include "include/utils.h"
#include "include/players.h"
#include "include/admin.h"
#include "include/hide.h"

class Hide final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	void AllPluginsLoaded();
	void* OnMetamodQuery(const char* iface, int* ret);
private:
	const char* GetAuthor();
	const char* GetName();
	const char* GetDescription();
	const char* GetURL();
	const char* GetLicense();
	const char* GetVersion();
	const char* GetDate();
	const char* GetLogTag();
};

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
