# Multiway — Bant yapısı (Claude Code görevi)

Multiway'e bant yapısı ekle. Mevcut DSP (STFT, history, faz düzeltmesi,
low-cut, mix, kuru ses hizalaması) çalışıyor, mimariyi koru.

Bantlar:
- 10 oktav bandı, merkezleri yaklaşık 31, 63, 125, 250, 500, 1k, 2k, 4k,
  8k, 16k Hz.
- Bant başına parametreler (APVTS, ID'ler band0_delayL ... band9_feedback):
  delayL (0–1000 ms), delayR (0–1000 ms), feedback (0–95 %).
- Her bin'in değeri, log-frekans ekseninde komşu iki bant merkezi
  arasında doğrusal interpolasyonla bulunsun (bant sınırında sert geçiş
  olmasın).

Genel parametreler: lowCut (mevcut), highCut (yeni, 200–20000 Hz,
logaritmik; maksimumda kapalı; kesimin 1/3 oktav ÜSTÜNE kadar yumuşak
geçiş, low-cut'ın aynası), mix (mevcut). Mevcut tek "delay" parametresini
kaldır.

Stereo: kanal 0 delayL, kanal 1 delayR tablolarını kullansın; her kanalın
kendi delayFrames ve phaseShift dizisi olsun.

Feedback: history'ye yazılan spektrum = mevcut giriş spektrumu +
feedback[k] × o bin için okunan gecikmiş değer. Bin başına feedback
0.95'i geçemesin.

Modülasyon için hazırlık: DSP bant parametrelerini doğrudan atomic'ten
değil, getEffectiveValue(paramIndex) gibi tek bir fonksiyondan okusun.
Şimdilik bu fonksiyon sadece parametre değerini döndürsün; ileride LFO
buraya eklenecek.

Tüm hesap frame başında, tüm bellek prepareToPlay'de. Editor şimdilik
GenericAudioProcessorEditor.

---

## Kontrol edilecekler (kod gelince)

- Bin interpolasyonu: bant merkezleri arasında log-frekansta doğrusal mı?
- Feedback satırı: history'ye yazılan değer, giriş + feedback × gecikmiş mi?
- High cut: maksimumda kapalı mı, rampa kesimin üstünde mi?

## Test

- Tüm bantlar aynı değerde → eski tek-delay davranışı.
- Alt bantlar 0 ms, üst bantlar 500 ms → bas hemen, tizler yarım saniye sonra.
- Feedback %50 → her tekrar yarı şiddette, sönümleniyor.
- L ve R farklı → stereo genişlik.
