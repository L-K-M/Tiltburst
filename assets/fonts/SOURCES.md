# Vendored font provenance (13-art-direction.md §5.1, 04-milestones.md M0)

Source repository: https://github.com/google/fonts
Pinned commit: `6a003b5eb672dc8bf5bff5937cf5863f8b175445`
Fetch date: 2026-08-25

| File | Upstream path at the pinned commit | License |
|---|---|---|
| `ChakraPetch-Bold.ttf` | `ofl/chakrapetch/ChakraPetch-Bold.ttf` | SIL Open Font License 1.1 (`ChakraPetch-OFL.txt`) |
| `Monoton-Regular.ttf` | `ofl/monoton/Monoton-Regular.ttf` | SIL Open Font License 1.1 (`Monoton-OFL.txt`) |
| `Righteous-Regular.ttf` | `ofl/righteous/Righteous-Regular.ttf` | SIL Open Font License 1.1 (`Righteous-OFL.txt`) |

Substitution note (03-process.md §3.2 fallback, ADR-015 in
docs/plan/02-decisions.md): upstream no longer ships a static
`Orbitron-Bold.ttf`, and vendoring the variable font is forbidden by
13-art-direction.md §5.1 (stb_truetype cannot instance it). The **orbitron
role** — geometric square sans for HUD/score numerals — is filled by
**Chakra Petch Bold**; the logical font name in code remains `orbitron`.

Integrity: `SHA256SUMS` (sha256sum -c format) is verified in-process by
`FontAssets.VendoredFontsPresentAndParse`.
