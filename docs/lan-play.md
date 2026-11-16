# LAN play and VPN discovery

The runtime's LAN mode uses the game's LAN lobby. It is off by default. For PCs on a VPN that does not carry
broadcast packets, `MKW_LAN_PEERS` also sends each lobby broadcast directly to the listed peers. This does not add a
public matchmaking service or Steam Remote Play; Steam's streamed local split screen is a separate feature.

## Start a private test

Both players need their own prepared installation, the same game version and compatible settings. Start with mods
off on both PCs; after a successful baseline race, test the same mods together. This feature does not download,
inspect or synchronize mods, and provides no anti-cheat system.

On the same home LAN, start with `MKW_LAN=1` and leave `MKW_LAN_PEERS` unset for ordinary broadcast discovery.
If discovery fails, or when using an already configured private VPN, each PC can list the other PC's private/VPN
IPv4 address, not its public internet address. For example, in a new PowerShell window in the installation folder:

```powershell
$env:MKW_LAN = '1'
$env:MKW_LAN_PEERS = '100.64.0.2'
.\mkw.exe --play
```

On the other PC use the first PC's address instead. A comma or space separates multiple peers (up to 16). A hostname
is resolved when the list changes; numeric addresses make a first test easier to diagnose. `MKW_LAN_IP` can select
the local adapter explicitly. Otherwise the route to the first peer selects it. Tailscale-range addresses use the
`100.64.0.0/10` mask presented to the game so its lobby does not reject the adapter's `/32`.

Choose **LAN Play**, create a group on one PC, and join it from the other. Check the logs for `lan: peer` and
`nifm: LAN` entries if the group does not appear. A firewall may need to permit the game on that private network;
the runtime does not install a VPN, change firewall rules or open router ports. Closing this PowerShell window
discards these environment overrides.

For a discovery failure, set `$env:MKW_LAN_DIAG = '1'` before starting. The log samples socket endpoints, packet
sizes and host errors, without payloads. This distinguishes a failed send from packets that arrive but do not
produce a room in the game. Leave it unset for normal play.

## Scope and verification

The guest socket layer continues to reject public IPv4 destinations, and its online-service resolver stays disabled.
Private address filtering is not authentication: access to the LAN/VPN and the choice of peers remain the players'
responsibility. This feature makes no account-ban guarantee and does not connect to Nintendo's online service.

The production socket overrides have been exercised with owned loopback UDP endpoints on Linux and Windows:
LAN opt-in, peer-list refresh and filtering, loopback binding, broadcast fanout to two endpoints, direct-send
isolation and rejection of public destinations. The harness is single-threaded and is not an end-to-end race test.
An external two-PC Tailscale race is reported in the source PR linked below; a race between the user's two PCs and
mod compatibility remain unverified here.

This fork preserves Gael50's implementation from upstream
[PR #345](https://github.com/adriadam10/mkw-recomp/pull/345), commit
`6b191f2e14ad4d492d0b4e4a3f2639e1e3a3ebe2`, with native socket tests and balanced temporary Winsock initialization
in the route query. Existing source authorship and MIT licensing are retained.
