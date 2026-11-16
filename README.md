## Usage

```sh
cmake -B build -G Ninja
cmake --build build
./build/quarrel
```

## Features

- **Logging in.** Email and password, a second factor from an authenticator app, a text message or a backup code, and hCaptcha when Discord asks for it. The token is kept in the Keychain.
- **Guilds.** The guild bar in the user's order with folders, unread marks, mention counts and names under the pointer.
- **Channels.** Categories that fold, text, voice, announcement, forum and stage channels, joined threads, unread and muted states, filtered by what the user may view.
- **Direct messages.** One to one and group conversations, with statuses, and the list of friends.
- **Messages.** Groups by author, date and unread dividers, replies, system messages, attachments, embeds, stickers, reactions, and the whole of Discord's markdown: headings, lists, quotes, code, spoilers, links, mentions, custom emoji and timestamps.
- **Writing.** Enter sends and shift with enter starts a line. Replies with or without a ping, editing the last message with the up arrow, deleting, reacting, and the typing notice.
- **Members.** The member list as the gateway syncs it, only the rows in view being built and asked for.

## Limits

- macOS only.
- No voice, no uploads, no search, no pins, no settings, no forum browsing and no threads the user has not joined.
- The password field takes characters at its end only.
- Images open in the browser, and animated ones show their first frame.
- Text in messages cannot be selected.
- A reply shows the plain text of what it answers, its markdown unread.
- Folders and the status come from Discord's JSON settings, which it marks deprecated. Without them the guilds show in the order Discord sent them.
