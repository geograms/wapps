# Torrents — design

The client half of `aurora/docs/torrents.md`. Read that first: it is the
protocol; this is the control surface over it.

## What this wapp is

A torrent client whose **unit of sharing is a folder, not a file**, whose
**address is a key** (`nfolder1…`, docs/torrents.md §11) rather than a hash of
the contents, and whose **tracker is the Indexer mesh** (aurora/docs/NOSTR.md).
Because the address is a key, the publisher can add or remove files and every
holder converges on the new state under the *same link* — no new magnet, no dead
torrent. The files inside stay content-addressed (sha256), exactly like
BitTorrent: the directory is mutable, the bytes are not.

## What lives where

Nothing about storage or networking is in here. The host owns it:

| Job | HAL |
|---|---|
| create a torrent from a disk directory | `hal_folder_add_disk` |
| list what we publish / what we follow | `hal_folder_list`, `hal_folder_subs` |
| one directory level of a torrent | `hal_folder_browse` (`"id\tpath"`) |
| file count, bytes, times served | `hal_folder_stats` |
| download one file / everything | `hal_folder_download` |
| **pin** (keep a full copy + announce as a holder) | `hal_folder_pin` |
| **the swarm** (who has this, and what are they made of) | `hal_folder_swarm` |
| **the share link** (`nfolder1…`) | `hal_folder_link` |
| republish what changed on disk | `hal_folder_rescan` |

`folder_pin`, `folder_swarm` and `folder_link` are new host HALs added with this
wapp. They are generic folder operations, not torrent-specific logic — the host
stays free of any app's vocabulary.

## Screens

- **Torrents** — what we publish ("Mine": we hold the key, so only we can change
  it) and what we follow, with file count, size and pin state. Tap to go inside;
  the same list then shows that torrent's directories and files.
- **Swarm** — who else has the open torrent, best holder first. Each row is a
  device and what it is *made of*: mains or battery, WiFi or cellular, hop count,
  how recently it was heard, and **whether we heard it ourselves or an Indexer
  told us** — after Indexer-to-Indexer sync the freshness being quoted is
  second-hand, and the age of the information is not the age of the device.
- **Info** — the `nfolder1…` link, the file count and size, and who holds the
  key. The link is copyable; the folder's *name* is deliberately not inside it
  (an unsigned name in a shareable string is a phishing surface — the real,
  signed name arrives with the op-log a second later).
- **Settings** — pin what I download (default on), and how often a disk-backed
  torrent is rescanned.

## Pinning is the whole point

Downloading gets you the bytes. **Pinning** keeps them, follows the op-log, and
tells the Indexers this device is a holder — which is what stops the publisher's
phone from being the only copy. A pin is a vote that the thing should survive.

Because of that, this wapp **autostarts in the background** (aurora's
`_defaultAutostartWappIds`): a seeder that only serves while its page is open is
not a seeder, and a pinned folder is a promise to the swarm that has to survive
the screen going off.

## Performance

The wapp runs in the background with no page attached, so the tick must be
near-free (aurora/docs/performance.md):

- **Every render is diffed before it is sent** (`changed_send` + djb2). An
  unchanged list is never re-pushed — a re-push costs a rebuild and resets the
  user's scroll.
- **The tick polls on long periods, not every second**: the list refreshes every
  6s, the disk rescan runs on a user-set interval (default 15 min).
- **The swarm is only resolved for the torrent the user has open.** A swarm
  refresh is a DHT walk; doing it for every row of the list would be a hot loop
  hidden behind a cosmetic number. The host caches the answer (60s TTL) and
  refreshes it in the background — including the *miss*, so a folder nobody holds
  does not re-walk the DHT on every render.
- One JSON object iterator (`next_obj`) for every list walk, so the parsing cost
  is the same everywhere and there is one place to get it right.

## Not built yet (and not faked)

The piece engine is the real torrent, and it is host-side work
(docs/torrents.md §8 step 2). Until it lands:

- a file is fetched **from one provider at a time**, not in pieces from many;
- a partial holder cannot seed what it already has (no piece bitfield);
- there is no endgame mode, no choking/tit-for-tat, no per-piece verification —
  a file is verified as a whole against its sha256.

So there is no rate limit, seed-ratio goal or sequential/streaming mode in
Settings, because there is nothing behind them yet. They arrive with the engine.
