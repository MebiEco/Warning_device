# Audio file manage + Azure download

**Date:** 2026-07-14  
**Status:** Approved

## Goal
Quản lý file âm thanh trên thẻ SD: tải từ Azure Blob (HTTPS/SAS), xóa, liệt kê — qua Azure Direct Method và trang web LAN.

## Commands (Azure method `Control`)
| Code | Action | Data |
|------|--------|------|
| 103 | Download | `Url`, `FileName` |
| 104 | Delete | `FileName` |
| 105 | List | (empty) → JSON files+sizes |

## Web
Tab Audio: list + refresh, delete, form Url+FileName download.

## Rules
- SD card only (`isSdCardReady`). Flash fallback ≠ ready for these ops.
- `FileName` user-defined; sanitize `[A-Za-z0-9._-]`; block `..` / `/`.
- Code 100: optional `FileName`; else `/sdcard/{PondId*100+DeviceId}.wav`.
- Shared download helper (HTTPS) used by Azure 103 and web.
