#pragma once

// Public API of the [AS] Hide module.
//
// Other Metamod plugins (spec lists, admin lists, HUDs, ...) can ask whether a
// player is currently hidden and skip them:
//
//	int ret;
//	IHideApi* pHide = (IHideApi*)g_SMAPI->MetaFactory(HIDE_INTERFACE, &ret, nullptr);
//	if (ret != META_IFACE_FAILED && pHide && pHide->IsClientHidden(iSlot)) continue;
//
// The admin system also receives the actions "hide_on" / "hide_off"
// (IAdminApi::OnAction), same as the original Hide module.

#define HIDE_INTERFACE "IHideApi"

class IHideApi
{
public:
	// true while the player is in hide mode
	virtual bool IsClientHidden(int iSlot) = 0;
};
