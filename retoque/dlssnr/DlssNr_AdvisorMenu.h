#pragma once

class Config;
namespace DlssNr
{
// Called even when the NR panel is not drawn; inactive is a single atomic gate.
void AdvisorMenuTick(Config* config, bool visible);
} // namespace DlssNr
