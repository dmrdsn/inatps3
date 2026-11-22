# M3U Player (PS3 homebrew)

CFW/HEN kurulu PS3 için basit bir M3U / IPTV oynatıcı. Kendi M3U listelerinizi
(USB bellekten ya da bir URL'den) açar, gruplara ve kanallara göz atmanızı sağlar,
HTTP/HTTPS ve HLS (`.m3u8`) yayınlarını oynatır.

> Yalnızca yayın hakkına sahip olduğunuz ya da yasal olarak erişebildiğiniz
> listelerle kullanın.

## Kurulum

1. GitHub Actions'tan inen `M3UPlayer.pkg` dosyasını USB belleğin köküne kopyalayın.
   (HEN kullanıyorsanız ve normal pkg kurulmazsa `M3UPlayer.gnpdrm.pkg` dosyasını deneyin.)
2. PS3'te **Package Manager → Install Package Files → Standard** ile kurun.
3. Uygulama XMB'de **Oyun** altında "M3U Player" olarak görünür.

## Listeleri ekleme

Uygulama açılışta şu konumları tarar:

| Konum | Ne okunur |
|---|---|
| `/dev_usb000` … `/dev_usb007` (USB kökü) | `*.m3u`, `*.m3u8` dosyaları ve `playlists.txt` |
| `/dev_usbXXX/m3u/` | aynı şekilde |
| `/dev_hdd0/game/M3UP00001/USRDIR/` | aynı şekilde (FTP ile kalıcı koymak için) |

`playlists.txt` her satırda bir liste içerir (`Ad | adres` ya da sadece adres):

```
# Bu satır yorumdur
Haber kanallarım | https://ornek.com/haber.m3u
Belgeseller | /dev_usb000/listeler/belgesel.m3u8
https://ornek.com/baska-liste.m3u
```

Desteklenen M3U etiketleri: `#EXTINF` (`group-title` dahil), `#EXTGRP`,
`#EXTVLCOPT:http-user-agent=` ve `#EXTVLCOPT:http-referrer=`.

## Kontroller

| Ekran | Tuş | İşlev |
|---|---|---|
| Listeler | ✕ | Aç / seç |
| | ◯ | Geri |
| | △ | Kanalı favorilere ekle/çıkar |
| | L1 / R1 (veya ← / →) | Sayfa atla |
| | L2 / R2 | Başa / sona git |
| | START | Uygulamadan çık (ana ekranda) |
| Oynatma | ↑ / ↓ | Önceki / sonraki kanal |
| | ✕ | Bilgi çubuğunu göster/gizle |
| | □ | Görüntü oranı: Sığdır / Ekrana yay / Yakınlaştır |
| | △ | Favori |
| | ◯ | Durdur ve listeye dön |

Japon düzeninde (◯ = onay) tuşlar otomatik olarak yer değiştirir.

Ana ekrandaki **Ayar** satırlarından HLS kalite sınırını (360p / 576p / 720p / en yüksek)
ve görüntü oranını değiştirebilirsiniz. Ayarlar ve favoriler `USRDIR` içine kaydedilir.

## Sınırlamalar

- Görüntü çözme işi PS3'ün PPU çekirdeğinde yazılımla (FFmpeg) yapılır. SD ve 576p
  yayınlar akıcıdır. 720p sınırda kalır, 1080p genellikle takılır. HLS listelerinde
  varsayılan olarak 576p ve altındaki varyant seçilir.
- Desteklenen kodekler: H.264, MPEG-2, MPEG-4 Part 2. Ses: AAC, MP2/MP3, AC-3/E-AC-3.
  HEVC/H.265 desteklenmez.
- Şifreli (AES-128) HLS, fMP4 segmentli HLS ve sonu `moov` ile biten (seek gerektiren)
  MP4 dosyaları desteklenmez. Canlı MPEG-TS ve TS segmentli HLS en iyi sonucu verir.
- HTTPS bağlantılarında sertifika doğrulaması yapılmaz (PS3'te güncel kök sertifika yok).
  TLS 1.3 zorunlu tutan sunucular çalışmayabilir.

## Derleme (GitHub Actions)

Depo GitHub'a gönderildiğinde `.github/workflows/build.yml` şunları yapar:

1. Hazır PSL1GHT SDK'sını indirir (bucanero/ps3toolchain, içinde curl, polarssl,
   freetype ve tiny3d var).
2. `scripts/build-ffmpeg.sh` ile yalnızca çözme yapan küçük bir FFmpeg 4.4 derler.
   Sonuç önbelleğe alınır.
3. `make pkg` çalıştırır ve `M3UPlayer.pkg` dosyasını **Artifacts** olarak yükler.

Yerelde derlemek için Linux/WSL'de `PS3DEV` ve `PSL1GHT` ortam değişkenleri
tanımlıyken aynı adımları uygulayabilirsiniz.

## Kaynak yapısı

| Dosya | İçerik |
|---|---|
| `source/main.c` | Arayüz, ekranlar, kumanda, ayarlar ve favoriler |
| `source/player.c` | FFmpeg çözücü iş parçacığı, PS3 ses portu, A/V senkronu |
| `source/net.c` | libcurl ile akış, HLS takibi, varyant seçimi |
| `source/m3u.c` | M3U ayrıştırıcı |
| `source/gfx.c` | Tiny3D, YUV doku çizimi (GPU'da renk dönüşümü), arayüz katmanı |
| `source/font.c` | PS3 sistem fontlarıyla FreeType metin çizimi |
