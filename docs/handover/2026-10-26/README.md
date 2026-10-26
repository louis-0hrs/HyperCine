# Handover 26.10.2026

Stand für die Erörterung mit dem Git-Controller. Alle Zahlen sind selbst gemessen
oder direkt aus dem Repo gelesen.

- **`main`**: `600cbf1`
- **Fünf offene PRs**: alle `MERGEABLE`, **0 rote Checks, 0 offene Checks**
- **Offenes Issue mit dem Diskussionsstand**: #257

## Die fünf PRs

| PR | Head | Umfang | Inhalt | Draft |
|---|---|---|---|---|
| #249 | `8a5b4d5` | +604/−25, 5 Dateien | A2 CUDA kompletter FilmScan3-Paket, bit-exakt | ja |
| #251 | `4a68408` | +346/−16, 5 Dateien | Mac Metal 4K: erreichbarer Geometry-Guard + echter Validator-Negativtest | ja |
| #252 | `4b5543f` | +149/−0, 1 Datei | 4K/8K/12K/17K Capability-Audit | ja |
| #255 | `2fbaebf` | +1013/−6, 9 Dateien | ARM64 NEON Wavelet bit-exakt + Dispatch + Performance | ja |
| #256 | `1ebe179` | +85/−9, 4 Dateien | CMake-Härtung, ersetzt das driftende #149 | **nein** |

`upstream/` ist in keinem PR berührt.

## Was heute auf `main` ging

- `9d290ab` — #250 Scheduler: generation-owned output + fail-closed nonblocking admission
- `600cbf1` — #254 SDK-Target-Guard bei deaktiviertem SDK

## Drei Fehler in meiner eigenen Arbeit

Diese stehen hier, weil sie das Vertrauen in alle anderen Zahlen betreffen.

### 1. 1080p als „4K" verkauft (#251)

`benchmarks/benchmark_metal_p2.cpp` allokierte `1920*1080*4` und übergab `7680`
als Stride an **jedes** Szenario. Die gesamte erste 4K-Tabelle war 1080p.

Beim Umbau kam ein zweiter Fehler heraus: `CpuMeter` hatte keine
`Geometry()`-Methode, also kodierte der CPU-Pfad 1080p, während Metal echtes 4K
kodierte:

```
CFHD byte=9 cpu_size=185592 metal_size=636548
```

Das sah nach einem Encoder-Bug aus, war ein Benchmark-Bug.

Echte Werte, Geometrie jetzt in jedem JSON-Record:

| Szenario | Geometrie | CPU median | Metal median |
|---|---|---|---|
| 2 (früher „4K") | 1920x1080 | 45,91 ms | 38,68 ms |
| 5 | **3840x2160** | **108,70 ms** | **76,13 ms** |
| 6 | 3840x2160 | 113,16 ms | 79,48 ms |
| 7 | 3840x2160 | 184,43 ms | 151,86 ms |

### 2. Ein Guard, der nie lief (#251)

In `tests/validate_filmscan3.py` stand die Prüfung „unchecked geometry is never a
pass" **nach** dem Encoder-Unterprozess. Bei einer Geometrie, die das Manifest
nicht enthält, ist `expected` leer, der Encoder wird mit null Eingaben
aufgerufen und stirbt an seiner Usage-Zeile. Der Guard war toter Code, und
„unbekannte Geometrie" sah identisch aus wie „Encoder-Crash".

Guard vor den Encoder-Aufruf gezogen.

### 3. Ein Negativtest, der nichts prüfte (#251)

Die erste Fassung prüfte nur `Exit != 0` und blieb **grün, als ich den Guard
entfernte** — der Encoder-Crash lieferte den Fehler. Erst nach Prüfung der
*Begründung* im Output wird der Test bei Mutation rot:

```
intakt   rc=0  GEOMETRY-NEGATIVE: PASS
mutiert  rc=1  rejected without saying 'NOT validated' (2 Fälle)
zurück   rc=0
```

Dazu ein vierter, kleinerer Fehler: der neue NEON-Benchmark gab **ungültiges
JSON** aus (Zeilen nur durch Newline getrennt, kein Komma). Aufgefallen durch
einen scheiternden Parser, nicht durch den Benchmark.

## Zwei Subagenten-Berichte, die falsch waren

### L6 (Capability-Audit) hat zweimal halluziniert

Gemeldet: Commit `5713825`, „8K SUPPORTED", „4K ~11,9 MiB pro Subband".

Realität:

```
git cat-file 5713825   -> not a valid object name
hypercine_encode_filmscan3 --geometry 7680x4320
  -> rc=1  "width [128,4096], height [24,4096]; 8K unqualified"
4K-Sample gemessen:      6.156.376 B bei 8.294.400 px = 0,742 B/px
```

Seine Subband-Größen sind um Faktor 2–12 daneben. #252 enthält die Widerlegung
mit namentlicher Korrekturliste.

### M3 (NEON) ist ehrlich gescheitert

„Das erforderliche host-aware `exec`/`nodes`-Tool ist nicht verfügbar. Jeder
verfügbare Shell-Zugriff würde auf Linux laufen."

Korrekt — und der Grund, weshalb ich die Arbeit danach selbst gemacht habe
(#255). Subagenten erreichen den Mac nicht, ich schon.

## Performance-Befund, der den Fokus verschiebt (#255)

Kernel: NEON **3,1–3,4×** schneller als Scalar. Ganzes Frame: **nur ~5,9 %**.

| Stage | 4K vorher | Anteil |
|---|---|---|
| Entropie | 86,4 ms | 48,0 % |
| Koeffizienten-Hashing | 42,7 ms | 23,7 % |
| Conversion | 19,8 ms | 11,0 % |
| **Quantisierung** | **19,5 ms** | **10,8 %** |
| Wavelet | 9,1 ms | 5,1 % |
| Framing | 2,5 ms | 1,4 % |

Quantisierung vektorisiert: **19,48 → 3,24 ms** (10,8 % → 2,0 %). Ganzes
4K-Frame 179,88 → 164,03 ms (~8,8 %). Abgesichert mit 1.457.305
Paritätsprüfungen.

Begründung für die Overflow-Sicherheit: größtes Produkt 2³⁰, größtes Ergebnis
16384 — 32-Bit-Lanes können nicht überlaufen, die Saturierung greift nie.

## Drei Entscheidungen, die offen sind

### 1. Hashing (23,7 % des Frames)

`coefficient_fnv64` ist eine diagnostische FNV-Kette über Koeffizienten und
landet **nicht** im Paket. Für Produktionsprofile wäre es streichbar — ein
Viertel der Frame-Zeit. Aber es ist der Beweismechanismus für CPU/Metal-Parität
und für die Validatoren.

**Frage:** hinter ein Profil-Flag, oder bleibt es Pflicht?

### 2. Entropie (48 %)

Bit-serieller Writer mit Run-Length-Flush. Vektorisierung hieße Umbau des
Bitstream-Writers — Formatrisiko.

**Frage:** lohnt das, oder ist 48 % akzeptabel?

### 3. NEON prescale-Pfad (45,8 % der Pyramide)

`standalone_encoder.cpp:160-162` nutzt `ForwardLevel(..., 2)` für Stufe 2. Dieser
Pfad rundet jeden Tap einzeln und ist deshalb bewusst skalar geblieben.
Vektorisierbar wäre es (`floor((x+3)/4) == (x+3)>>2`, erschöpfend geprüft),
Deckel ist aber ~1,4 % des Frames.

**Frage:** machen oder als dokumentierte Grenze stehen lassen?

## Nebenbefund ohne Issue

`EncodeBgra` kann **4K gar nicht**: `kMaxPixels = 2097152` liegt unter 4K UHD
(8.294.400), wirft `length_error`. 4K muss über `FilmScan3Encoder` mit
expliziter Geometrie laufen.

Aufgefallen, weil mein erster Benchmark daran abbrach. Ob Legacy-Absicht oder
offene Lücke, ist nicht dokumentiert.

## Decoder-S3: Entwurf gesichert, nicht fertig

Der Entwurf liegt in `decode-s3-draft/` — **absichtlich nicht** als Build-Code,
weil der Test 471 Compile-Fehler hat und `CMakeLists.intended.txt` ihn
registrieren würde.

| Datei | Zustand |
|---|---|
| `decode_plane_contract.h` (201 Z.) | **kompiliert** (`-fsyntax-only`, 0 Fehler) |
| `wavelet_consumer.h` (177 Z.) | scheitert nur an einem Include-Pfad |
| `test_decode_s3_contract.cpp` (470 Z.) | **471 Compile-Fehler** |
| `CMakeLists.intended.txt` | die beabsichtigte Registrierung, nicht aktiv |

Der Entwurf enthält: `TypedPlane`, `SignedPlane`, `BufferBudget`,
`BandMapper`, `BoundedWaveletConsumer`, `ConsumerResult`.

Der Test war für acht Regeln gedacht: `clean_graph_succeeds`, `dim_mismatch`,
`invalid_pitch`, `missing_band`, `empty_label_rejected`, `consumer_failed`,
`downstream_not_completed`, `downstream_sees_data`, plus sieben
Mutationskontrollen (`mut1_wrong_channel` … `mut7_error_publish_fail`) und
Worker-Skalierung 1/2/4/16.

## Bilanz der sechs Delegationen

| Strang | Ergebnis |
|---|---|
| L1 (CUDA/mkdtemp) | geliefert, selbst verifiziert (#249) |
| L2 (Scheduler S2) | geliefert, selbst verifiziert, **gemergt** (#250) |
| L4 (CMake) | geliefert, selbst verifiziert (#256) |
| L6 (Capability) | **zweimal halluziniert**, widerlegt (#252) |
| L2 (Decoder-S3) | **nichts geliefert** (2 h 29 min, 470k Tokens) |
| M3 (NEON) | ehrlich gescheitert, von mir selbst gemacht (#255) |

Zwei von sechs unbrauchbar, einer davon mit erfundenen Zahlen.

## Offen außerhalb meines Bereichs

- **#244 / #237**: Windows-native Testabnahme, liegt beim Windows-Owner.
- **#149** ist noch offen (non-draft, `543ba5d`) und wird durch **#256** ersetzt.
  Soll es geschlossen werden?
- **29 offene Drafts** insgesamt im Repo.
