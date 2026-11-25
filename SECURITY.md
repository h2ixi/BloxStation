# Security

## What is stored, and where

Everything stays on the console, in `/data/roblox/files/`:

| File | What it is |
|---|---|
| `session.txt` | your Roblox sign-in cookies (the same as a browser keeps) |
| `warp.json` | this console's tunnel key and Cloudflare registration |
| `warp-endpoint.txt` | the tunnel server that worked last (not secret) |
| `mouse-games.txt` | games where the touchpad acts as a mouse |

Keys are created with the console's random device, written with mode 0600
and never overwritten. If a tunnel registration fails halfway, the title
resumes it later instead of creating another one.

## What leaves the console

- To Roblox: the same requests the official Android app makes.
- To Cloudflare (`api.cloudflareclient.com`, certificate checked): this
  console's public key once, on first launch. Then the encrypted tunnel.
- Nothing is sent to the project, to a PC or to any other server.

## Rules for contributors

- Never commit sign-in cookies, `warp.json`, console logs (`radv-smoke.txt`)
  or Roblox app files. `.gitignore` excludes the usual names, but check
  `git status` before every commit.
- Logs print cookie and key names, never their values; keep it that way.
- Do not add code that defeats Roblox's anti-cheat or account checks.

## Reporting

Open a private security advisory on GitHub rather than a public issue.
