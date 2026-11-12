# Multiplayer (Link Play) — experimental

Pokopia's **Link Play** (local wireless, several players on one island) between PCs, Steam Decks and other handhelds. This port speaks
pokopia-native's co-op protocol, so you can also play with **pokopia-native** players.

## Before you start

- **Cross-play** works with pokopia-native players on game version **2.0.1**. Players on 1.0.5 cannot join (the game builds differ).
- **Not compatible with earlier Link Play builds of this port** (the old relay on port 39660). Everyone needs a build with this protocol.
- **Each player needs their own save.** The host refuses a visitor whose save it already has (error 9005, "Communication error"):
  two copies of one save cannot play together. Earlier builds gave every new save the same player id, so every visit failed with
  9005; the game now repairs such a save by itself at start (a copy is kept in `uuid-backup\` in the save folder).
- Every player needs the same **Interact everywhere** and **Story co-op** settings, or the host is not listed. Only the host's save keeps story progress.

## Setting it up

Use the launcher's **Link Play** page. It writes the `[net]` and `[pkp]` keys in `pkp.ini` for you; changes apply the next time the game
starts. At the top of the page, pick one:

| Choice | What it does |
|---|---|
| Off | No Link Play. |
| Same network | The default. On the same Wi-Fi or network the host is found by itself. **Join directly** takes one host's address (shown under **This PC** on the host's page) when it is not found. |
| Internet | **Host** or **Visit** over the internet with a room code, through the project's public relay. Nothing else to set up. Your own relay or a forwarded port are under **Advanced**. |

The page shows the menu path to take in the game. In the game: the PC in your first house -> Link Play (unlocked after Environment
Level 2 in Withered Wasteland) -> **Local connection**. **Online connection** is Nintendo's own service and does not work here. The host
picks **Invite Others to Visit**, the others **Visit a Friend**.

**Same for everyone:** **Visitors can build everywhere** (moves, items and building in the story areas too, not only in Palette Town) and
**Visitors can talk to story characters** (turns the first one on). Both are experimental.

## Step by step: on the same network

1. Everyone updates to the same release and starts the launcher.
2. Everyone opens **Link Play** and picks **Same network**. Leave **Join directly** empty.
3. Everyone presses **Play**. In the game: PC -> Link Play -> Local connection.
4. The host picks **Invite Others to Visit**. The visitors pick **Visit a Friend** and choose the host from the list.
5. The host is not in the list? The host reads the address under **This PC** on their Link Play page (for example `192.168.1.20:47600`).
   Each visitor types it in **Join directly**, closes the game and starts it again.

## Step by step: over the internet

The launcher uses the project's **public relay** by default: every player dials out to it, so nobody forwards a port or types an address.

1. Everyone updates to the same release and starts the launcher.
2. Host: **Link Play** -> **Internet** -> **Host**. Press **New code** if you want a fresh one (it stays the same until you press it again)
   and send the room code to your friends (the copy button helps).
3. Visitor: **Link Play** -> **Internet** -> **Visit**. Type the code in **Room code**.
4. Everyone presses **Play**. In the game: PC -> Link Play -> Local connection. The host picks **Invite Others to Visit**, the visitors
   **Visit a Friend**.

**Advanced: without the public relay.** Under **Internet** -> **Advanced**, turn on **Use my own relay or address**. Then pick one way;
both need one change on the router of the player who hosts (a port forward).

**Way A: the host runs a relay.** Only the host's router changes.

1. Host: on the router, forward port **47610**, TCP and UDP, to the host's PC. (Search your router's model and "port forwarding" for
   the steps; the address to forward to is the one under **This PC** on the **Same network** view, without the `:47600`.)
2. Host: **Host**, then under **Advanced** turn on **Run a relay on this PC**. Keep the launcher open while you play: the relay stops
   when it closes.
3. Host: under **Share with visitors** the page shows the address to give out, for example `203.0.113.5:47610`. Send your friend that
   address and the room code.
4. Visitor: **Visit**, then under **Advanced** type the address in **Address** and turn on **Through a relay**. Type the code in **Room code**.
5. Both press **Play** and go in the game as above.

Any other relay works the same way: the host types its address in **Relay address** instead of running one, and shares it.

**Way B: no relay.** The host's router forwards the game's own port instead.

1. Host: on the router, forward port **47600**, TCP and UDP, to the host's PC.
2. Host: **Host**, then under **Advanced** leave **Relay address** empty and **Run a relay on this PC** off. Send your friend the address
   under **Share with visitors** (for example `203.0.113.5:47600`) and the code.
3. Visitor: **Visit**, then under **Advanced** type the address and leave **Through a relay** off. Type the code in **Room code**.
4. Both press **Play** and go in the game as above.

**It still does not connect?**

- Both on the same release, and the same two **Same for everyone** switches? If not, the host is not listed or the visit is refused.
- The first time you host, Windows asks about `pkp.exe` (and, with Way A, about the launcher, which runs the relay): allow both. If you
  said no, allow them in Windows Firewall.
- Way A or B: check the forward with a port-check website while the relay (Way A) or the game (Way B) runs on the host. If the port shows closed,
  the router forward or the firewall is the problem. Some internet providers (CGNAT, many mobile and some fibre connections) do not allow
  forwarding at all; then the other player hosts, or the host asks the provider for a public IPv4 address.
- Never send `127.0.0.1` or an address starting with `192.168.`, `10.` or `172.`: those only work at home. The visitor needs the address
  under **Share with visitors**.
- Each player plays their own save (see Before you start).
- Still stuck: open an issue with both players' `pkp.log`.

**Steam Deck and other Proton/Linux devices** find hosts and are found the same way. Proton has no DNS-SD, so there the game sweeps
its own network for hosts on port 47600 each time it scans, and a hosting device answers DNS-SD (mDNS) queries itself, so Windows players
see it too. Nothing to type, and nothing to change when you take the device to another network. `[net] lan_sweep = off` turns this off.

What it cannot get past:
- **Wi-Fi that keeps devices apart** (many cafes, hotels and guest networks: "client isolation"). Nobody on it can reach anybody. Use
  **Internet** with a room code instead.
- **A host on another port.** When 47600 is taken on the host (a second copy of the game), it listens on a port the system picks, and the
  sweep does not find it. Visit it by address with the port from its log.

## Invites and Tailscale

Instead of typing an address and a room code, the host can **copy an invite** and the friends **paste** it.

- **Host:** on the launcher's Link Play page, press **Copy invite** beside an address (a same-network address, a Tailscale address, the
  public address, or a relay). For Tailscale or the internet the invite includes the room code, so the host needs one: **New code**, or
  start the game as host. Send the whole message to your friends in any chat.
- **Friend:** open the Link Play page and paste the message into **Paste invite**. The launcher fills in the address and the room code and
  picks the right mode (Same network, or Internet with Visit), then the usual steps in the game apply.
- **Tailscale:** the game counts Tailscale's addresses (100.64.0.0/10) as internet play, so every player needs internet play and the
  host's room code. The invite does this for you: on the host's Internet page, with **Through a relay** off, the page lists the host's
  Tailscale address; **Copy invite** next to it, and the friends paste. Tailscale has no automatic discovery in the game (the LAN sweep
  skips VPN adapters), so the address always travels in the invite.
- An invite is plain text and carries the room code: share it only with the people you want to play with.

Three read-only helpers on the same page (they change nothing on your PC):

- **Test connection** (Visit and Same network): tries to reach the address you typed and says what happened in words: reached,
  the PC answered but no game is hosting yet, no answer (a firewall may block the port, or it is on another network), or not an address.
- **Check my setup:** looks at this PC's addresses, the Windows network profile and, on Linux, what the firewall says, and lists what may
  stop friends from joining, with a short fix for each. It never changes a firewall or network setting.
- **Friends on my Tailscale** (Visit): if Tailscale is installed, lists your devices and which of them are hosting right now; **Join**
  fills in that device's address, then paste the host's invite (or type the room code) to finish.

## Ports and firewall

| Port | Who | What |
|---|---|---|
| 47600 TCP + UDP | the host | Visitors connect here (LAN, or the internet without a relay). |
| 47610 TCP (+ UDP) | the relay | Internet play through a relay (the public one, or your own). UDP is optional, see below. |

- The first time you host, Windows Firewall may ask about pkp.exe: allow it on **private** networks. If nobody finds you, check that your
  network is set to Private (Settings, Network & internet, your connection, Network profile type) and that pkp.exe is allowed.
- **Internet through the public relay** (the default): every player only dials out; nothing to forward.
- **Internet without a relay** (Advanced): the host forwards TCP and UDP 47600 on its router; visitors pick **Internet** -> **Visit** with the host's public
  address and the host's room code, with **Through a relay** off.
- **Internet through your own relay** (Advanced): nobody forwards anything except whoever runs the relay. Any pokopia-native coop relay works. The launcher can
  run one too: **Run a relay on this PC** on the Link Play page (while the launcher stays open), or `PokopiaLauncher --host` on a server
  (`--listen <IPv4>[:<port>]`, `--no-udp`, and pokopia-native's limits options), or the standalone `pokopia-relay` server
  (`launcher/relay/README.md`). Forward TCP 47610 to it, and UDP 47610 too for the faster path.

With the launcher's relay and this port's build on both ends of a link, the game's packets go over UDP through the relay instead of the TCP
link, which avoids stalls on lossy connections. With pokopia-native's relay, or a pokopia-native player on the other end, they use the TCP
link as pokopia-native does. Either way they are checked end to end with the room code: the relay only forwards them. Nothing is encrypted.

## Old protocol

The old protocol is `[net] protocol = linkplay` in `pkp.ini`, for a session where everyone still has an earlier build of this port. It
cannot meet pokopia-native players. When the file says it, the Link Play page shows a banner with **Switch to current**.

## Common problems

- **The host is not found:** type the host's address in **Join directly** (Same network). Check the firewall prompt and the Private network profile.
  On cafe or hotel Wi-Fi that keeps devices apart, nothing on the LAN works: use **Internet**.
- **Refused:** version or mods mismatch. Same game (2.0.1), same release or pokopia-native, same two switches.
- **Error 9005:** both players have the same player id: two copies of one save, or a player still on an earlier build (every save made
  before had the same id). Update both; a copied save still needs a new game.

## Status

Joining and walking around together work. Not yet tested: long sessions, more than two players, leaving and rejoining, play over the
internet. Expect bugs; report them with every player's `pkp.log`.
