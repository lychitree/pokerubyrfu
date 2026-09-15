#include "global.h"
#include "link.h"
#include "event_data.h"

// Script-callable wrapper around the real IsWirelessAdapterConnected (in
// link.c) for the Union Room prototype's Link Room gating. Renamed to avoid
// colliding with that function -- Emerald already uses the name
// IsWirelessAdapterConnected for the actual hardware-detection check the RFU
// connection layer and main menu depend on.
void ScrSpecial_IsWirelessAdapterConnected(void)
{
    gSpecialVar_Result = IsWirelessAdapterConnected();
}
