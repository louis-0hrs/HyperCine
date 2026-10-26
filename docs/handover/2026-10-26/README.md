# Handover 26.10.2026 — Stand 14:00

Für die Erörterung mit dem Git-Controller. Alle Zahlen sind selbst gemessen oder
direkt aus dem Repo gelesen. Wo etwas nicht geprüft ist, steht das dabei.

- **`main`**: `260eab0`
- **Sieben offene PRs**, alle `MERGEABLE`, alle Draft
- **0 rote Checks zum Zeitpunkt dieses Schreibens** — aber siehe „Was noch läuft"

## Die sieben PRs

| PR | Head | Umfang | Inhalt |
|---|---|---|---|
| #249 | `9e054d9` | +891/−34, 6F | CUDA A2: `main` gemergt, 4K + 16 Frames belegt |
| #251 | `99a2a48` | +299/−16, 5F | Metal 4K: `main` gemergt, echte Messung statt Relabeling |
| #255 | `5491899` | +1408/−6, 10F | ARM64 NEON: atomar, TSan, Parität bis 4K |
| #260 | `5825380` | +666/−0, 5F | Decoder-S3: minimaler typisierter Vertrag |
| #261 | `48b43d7` | +139/−11, 8F | Hashing als Session-Diagnose, fail-closed |
| #262 | `0499323` | +471/−2, 2F | Rate-State: 30-fps-Golden, 65.535-Stopp |
| #263 | `59611bb` | +1148/−1, 7F | Capability-Contract, in Build und CTest |

`upstream/` ist in keinem PR berührt. Nichts gemergt, kein Force-Push, kein Tag.

## Heute auf `main` gegangen

- `9d290ab` — #250 Scheduler S2
- `600cbf1` — #254 SDK-Target-Guard
- `481f96c` — #256 CMake-Härtung (ersetzt #149, das geschlossen wurde)
- `260eab0` — #252 Capability-Audit (vom Controller direkt korrigiert)

## Drei Fehler in meiner eigenen Arbeit

Diese stehen hier, weil sie das Vertrauen in alle anderen Zahlen betreffen.

### 1. Vier CI-Checks mit dem #249-Merge zerbrochen

Ich stellte beim Forward-Merge den CUDA-Block **vor** die Policy-Guards. Auf Hosts
ohne nvcc starb der Configure mit `HYPERCINE_ENABLE_CUDA is ON, but no CUDA compiler
was detected` — und maskierte damit genau die Meldung, die
`upstream_off_configure_contract` erwartet:

```
AssertionError: 'CUDA requires HYPERCINE_BUILD_UPSTREAM=ON' not found in
'... CMake Error at CMakeLists.txt:49: ... no CUDA compiler was detected'
```

Betroffen: `Windows MSVC (CPU-Only)` ×2, `Quality state (Linux)`, `Quality state
(Windows)`. Behoben in `f391ab0`: beide Guards laufen jetzt vor der
Compiler-Erkennung. Lokal alle vier Konfigurationen verifiziert.

### 2. Sentinel-Test brach Windows (`os.readlink`)

`os.readlink(link) == str(real_target)` ist eine POSIX-Annahme. Windows liefert das
Ziel in eigener Form, der String-Vergleich scheitert bei intaktem Symlink. Behoben
in `9e054d9` durch `resolve()`-Vergleich.

Nebeneffekt: dabei fiel auf, dass der Filter für „bestehende Einträge" auf
Vorwärtsschrägstriche matchte — auf Windows prüfte er dadurch fast nichts. Coverage
stieg von 30 auf **31 Checks**, weil der Test jetzt wirklich prüft.

### 3. Shell-Quoting brach Windows erneut (#263)

Mein Portabilitäts-Fix für L2s Test machte die **Pfade** portabel, nicht die
**Shell-Syntax**. `Run()` baute `command + " >'" + path + "'"` — für `cmd.exe` sind
Single-Quotes literal:

```
FAIL "C:/ProgramData/chocolatey/bin/ffmpeg.exe" -version -> 1
27/62 encoder_capability_contract ***Failed
```

Zwei Windows-Runner rot. Behoben in `59611bb` mit einem `Quote()`-Helfer
(`"..."` auf `_WIN32`, `'...'` sonst).

**Muster:** Zweimal derselbe Fehlertyp an einem Tag — POSIX-Annahme, lokal grün,
Windows rot. Beide in Test-Harnesses, nicht im Produktcode.

## Was selbst verifiziert wurde, nicht geglaubt

Jedes Subagenten-Ergebnis wurde nachgefahren:

| Strang | Prüfung | Ergebnis |
|---|---|---|
| L5 Decoder-S3 | Extent-Prüfung entfernt | `holds=8 failures=2` |
| L4 Hashing | eigenes Prüfprogramm, 4 Kombinationen | `off vs off` → **0** (kein stiller Pass) |
| L3 Rate-State | Timecode um 1 verschoben | `30fps golden bytes changed` |
| L2 Capability | selbst gebaut + ausgeführt, dann CTest | 4K 636612 B, 62/62 |
| M1 NEON | NEON-Rundung um 1 | `FAIL (5750 mismatches)` |
| M1 NEON | TSan-Lauf | 0 Warnungen, exit 0 |

## Die Zahlen, die zusammengehören

### NEON: Kernel schnell, Frame kaum

| Scope | Geometrie | Scalar | NEON | Speedup |
|---|---|---:|---:|---:|
| Kernel | 1920×1080 | 1,7463 ms | 0,5136 ms | 3,400× |
| Kernel | 3840×2160 | 6,8093 ms | 2,1975 ms | 3,099× |
| **Encode** | 1920×1080 | 43,309 ms | 40,737 ms | **1,0631× (5,94 %)** |
| **Encode** | 3840×2160 | 173,272 ms | 162,981 ms | **1,0631× (5,94 %)** |

3,1–3,4× auf dem Kernel, **5,94 % auf dem Frame** — identisch bei beiden
Geometrien. Wer „NEON macht den Encoder mehrfach schneller" sagt, irrt.

### Hashing: gleiche Zeit, anderer Nenner

| Pfad | 4K-Frame | Hash | Anteil |
|---|---:|---:|---:|
| CPU standalone (#261) | 379–703 ms | 43–47 ms | 6–12 % |
| Metal-Pfad (#251) | 76,19 ms | 42,70 ms | **56 %** |

Die **absolute** Hash-Zeit ist gleich (~43–47 ms), weil es derselbe Code über
dieselbe Koeffizientenzahl ist. Nur der Nenner unterscheidet sich. Eine der beiden
Zahlen ohne ihren Pfad zu zitieren führt in die Irre.

## CUDA (#249): die Belege

Geometrie-Matrix auf der A2, jede Zeile ein vollständiger Lauf mit echtem
FFmpeg-Decode:

| Geometrie | Paket | Hashes | FFmpeg |
|---|---:|---|---|
| 128×24 | 8.756 | 30/30 | PASS |
| 1280×720 | 1.707.164 | 30/30 | PASS |
| 1920×1080 | 3.833.564 | 30/30 | PASS |
| 2048×1080 | 4.089.468 | 30/30 | PASS |
| **3840×2160** | **15.325.284** | 30/30 | **PASS** |

4K: `FULL_PACKET: PASS (15325284 bytes byte-equal)`,
`yuv_sha256=c67fd5f19db9d24fda8b2e991c8dc90428a2e332af0911473c14286b7f252bb2`.

16-Frame-Session bei 1080p: `MULTIFRAME: PASS (16 ordered frames, 51045800 bytes
total, rate state and frame counter identical)`. Limiter läuft 4→9→14→17→20,
16 verschiedene Byte-Zahlen.

Das Werkzeug hatte `1920×1080` an drei Stellen hart kodiert — das Frontend war
längst geometriefähig, das **Werkzeug** war die Grenze.

## Metal (#251): gemessen, nicht übernommen

Native M2 Pro, `main` gemergt, **68/68 ctest**. Benchmark reproduziert:

| Szenario | Geometrie | CPU median | Metal median |
|---|---|---:|---:|
| 5 | 3840×2160 | 108,61 ms | 76,19 ms |
| 6 | 3840×2160 | 113,15 ms | 79,78 ms |
| 7 | 3840×2160 | 184,53 ms | 151,90 ms |

`packet_equality: PASS`, Peak RSS 347,2 MiB.

Die Stage-Tabelle war als „owner-reported" markiert. Sie ist jetzt durch eigene
Messung ersetzt, mit der Prüfung, die zählt:

```
Metal-Seite + geteilter Writer = 76,16 ms
gemessener Metal-Median         = 76,19 ms   (Δ 0,03)
alte Doku-Tabelle              = 75,78 ms   (Δ 0,35)
```

`batch_gpu_ms` (3,97) liegt **innerhalb** von `wait_ms` (4,66), Rest 0,69 ms — nicht
addieren.

## NEON (#255): was der Schalter wirklich kostet

Der Data Race ist behoben (`std::atomic<bool>`, release/acquire). TSan:

```
TSAN_RC=0
WARNING: ThreadSanitizer  -> 0 occurrences
NEON-CONCURRENCY: PASS (6 checks, 0 failures)
```

**Aber der Schalter ist weiter prozessweit, nicht pro Session.** Mein erster
Testversuch wollte die Interferenz an Paketbytes beweisen — das ist **prinzipiell
unmöglich**, weil beide Kernel bit-exakt sind. Der Fehlschlag war der Befund:

- Interferenz ist **kein Korrektheitsproblem** — jede Session bekommt ein valides,
  byte-identisches Paket, egal welcher Kernel lief.
- Sie **ist** ein Nachweisproblem — eine Session, die NEON gemessen hat, kann nicht
  versprechen, dass der nächste Lauf NEON nutzt.

Die Lösung gehört in #263s `EncoderSessionConfig`.

Vollständige Parität jetzt auf echten Geometrien (vorher nur 128–512, weil
`EncodeBgra` bei `kMaxPixels = 2.097.152` endet): 128×24, 1280×720, 1920×1080,
2048×1080, **3840×2160**, plus 8-Frame-Sequenz. **1.457.338 Checks.**

## Decoder-S3 (#260)

Zehn Regeln halten, `DECODE-S3 PASS holds=10 failures=0`. Stufen 1–6 fertig:
typisierter Signed-16-Plane (Geometrie, 32-Byte-Stride, Byte-Extent, 2 MiB Deckel),
ein positiver Buffer-Test, ein negativer Extent-Test, Kanal/Band-Mapping, begrenzter
Consumer, Generation/Ownership/Cancellation über die gemergte S2-Schicht.

Mutationsgeprüft: Extent-Vergleich entfernt → `holds=8 failures=2`.

Nicht drin: kein GoPro/CFHD-Decoder, kein Pixel-Orakel, `publish()` lehnt zu kurzen
Span nicht vor dem Version-Store ab.

## Capability (#263)

Von L2 geliefert, von mir in Build und CTest eingehängt — es war vorher **in keiner
CI**. Der Bericht nannte die Lücke selbst.

Geometrie-Matrix mit echtem FFmpeg-Decode: 128×24, 256×256, 1280×720, 1920×1080,
2048×1080, 3840×2160 (636.612 B, decoded 33.177.600).

Grenzfälle vor jeder Allokation:

| Anfrage | Ergebnis |
|---|---|
| 4096×2024 | im Budget, aber `BackendNotQualifiedForGeometry`, kein Encode |
| 4096×2160 | Pixelbudget verletzt |
| 7680×4320 | außerhalb der Dimensionshülle |

Nicht behauptet: keine Metal/CUDA/Vulkan-Qualifikation, kein SDK-Orakel. Die
Speicherdecke `36*Pixel+1 MiB` ist Planung, kein gemessenes RSS.

## Rate-State (#262)

30-fps-Golden im Test verankert: `sha256 d75e6af7…`, Größe 185.648. Mutationsbeweis:
Timecode um 1 verschoben → `30fps golden bytes changed`.

Tag 69 (`frame_index+1`, 16 Bit), `UFRM` (32-Bit-LE, 0-basiert) und `TIMC` (11 Byte)
sind getrennte Felder — bewiesen, indem nur `TIMC` überschrieben wird und 4 Bytes
wechseln, während Tag 69 und UFRM bei 1 und 0 bleiben.

65.535-Stopp ist ein Stopp, kein Wrap: Index, Limiter und `previous_sample_bytes`
bleiben stehen, `FilmScan3 frame counter exhausted`.

## Vier Fragen, die eine Produktentscheidung brauchen

Aus #262, nicht von einem Implementierer zu entscheiden:

1. Darf Tag 69 nach explizitem `Reset` mitten in einer Datei wieder bei 1 starten?
2. Darf `UFRM` über 65.535 weiterzählen, während Tag 69 stoppt?
3. Liegt Drop-Frame außerhalb der durchsuchten Quellen? (Nicht gefunden, nicht
   erfunden.)
4. Gibt es eine belegte FilmScan3-Abbildung für 50, 60 oder `60000/1001`?

## Was noch läuft

Die CI auf den letzten Heads war zum Zeitpunkt dieses Schreibens nicht überall
durch. Insbesondere: **der Windows-Runner hat meine Quoting-Korrektur (`59611bb`)
noch nicht bestätigt.** Das ist die eine Behauptung hier, die noch nicht bewiesen
ist. Der Fehlermodus war eindeutig (literal Quote an ffmpeg), aber grün ist erst
grün, wenn der Runner es sagt.

## Bilanz der neun Delegationen

| Strang | Ergebnis |
|---|---|
| L1 CUDA/mkdtemp | geliefert, selbst verifiziert (#249) |
| L2 Scheduler S2 | geliefert, verifiziert, gemergt (#250) |
| L2 Capability | geliefert, Lücke geschlossen (#263) |
| L3 Rate-State | geliefert, Mutationsbeweis nachgeholt (#262) |
| L4 Hashing | geliefert, Gate selbst geprüft (#261) |
| L5 Decoder-S3 | geliefert, Mutation selbst geprüft (#260) |
| L6 Capability-Audit | **zweimal halluziniert**, widerlegt (#252) |
| M1 NEON | von mir selbst gemacht (#255) |
| M3 NEON | ehrlich gescheitert (kein Mac-Zugriff im Subagent) |

Zwei von neun unbrauchbar, einer davon mit erfundenen Zahlen.
