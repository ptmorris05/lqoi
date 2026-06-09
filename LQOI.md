### **QOI & LQOI Technical Reference**

#### **Part I: Original QOI (Quite OK Image) Format**

QOI is a greedy, single-pass, lossless image compression algorithm. It operates row-major and maintains minimal state, achieving extreme encode/decode speeds on single-core CPUs.

**Global State:**

1. **Previous Pixel (**px\_prev**):** Initialized to r:0, g:0, b:0, a:255.  
2. **Hash Array (**index\[64\]**):** A 64-element array of previously seen pixels, zero-initialized.  
   * **Hash Function:** (R × 3 \+ G × 5 \+ B × 7 \+ A × 11\) mod 64

**File Structure:**

* **Header (14 bytes):** Magic qoif (4), Width (4), Height (4), Channels (1), Colorspace (1).  
* **Data Stream:** Sequence of variable-length, byte-aligned chunks.  
* **End Marker (8 bytes):** 0x00 (x7) followed by 0x01.

**Encoding Methods (Opcodes):**

* **QOI\_OP\_RUN (**11xxxxxx**):**  
  * *Payload:* 6-bit run length (1 to 62), biased by \-1.  
  * *Trigger:* Current pixel exactly matches px\_prev.  
* **QOI\_OP\_INDEX (**00xxxxxx**):**  
  * *Payload:* 6-bit index (0 to 63).  
  * *Trigger:* Current pixel exactly matches index\[hash\].  
* **QOI\_OP\_DIFF (**01xxxxxx**):**  
  * *Payload:* 2-bit ΔR, ΔG, ΔB.  
  * *Trigger:* Δ for RGB are each within \[-2, 1\]. Stored with \+2 bias.  
* **QOI\_OP\_LUMA (**10xxxxxx**):**  
  * *Payload:* Byte 1: 6-bit ΔG \[-32, 31\]. Byte 2: 4-bit ΔR \- ΔG \[-8, 7\] and 4-bit ΔB \- ΔG \[-8, 7\].  
  * *Trigger:* ΔG and relative ΔR, ΔB fall within respective ranges. Stored with \+32 and \+8 biases.  
* **QOI\_OP\_RGB (**11111110**) / QOI\_OP\_RGBA (**11111111**):**  
  * *Payload:* 3 or 4 full bytes of raw channel data.  
  * *Trigger:* Fallback when no other opcode applies.

#### **Part II: Lossy QOI (LQOI) Implementation Specification**

LQOI introduces perceptual quantization directly into the encoding logic. The following specifications detail the exact mathematical and state changes required to implement the lossy variant.

**1\. Green-Weighted Manhattan Distance (Global Error Function)**

* **Mechanism:** Replaces exact-match checks with a branchless L1 norm weighted heavily toward the Luma (Green) channel.  
* **Formula:** E \= 2 × |ΔG| \+ |ΔR| \+ |ΔB|  
* **Threshold:** E ≤ 6 (Alpha channel must always exactly match; ΔA \= 0).

**2\. Chroma-Biased Lossy Runs**

* **Mechanism:** A run continues as long as the current source pixel's error E relative to the *initiating base pixel* of the run is ≤ 6.  
* **Implementation:**  
  * Store px\_base \= current\_px when a run begins.  
  * For subsequent pixels, if (2 × |px.g \- px\_base.g| \+ |px.r \- px\_base.r| \+ |px.b \- px\_base.b|) ≤ 6 AND px.a \== px\_base.a, increment run.  
* **Time:** Decreased (bypasses hash/diff math).  
* **Compression:** Massively increased.  
* **Error:** Strictly bounded by E ≤ 6 relative to the start of the run, preventing compounding drift.

**3\. Lossy Indexing (Snap to Palette)**

* **Mechanism:** Allows a pixel to "snap" to a perceptually similar color already residing in the hash array by using a **locality-sensitive hash**. This ensures similar colors collide into the same bucket.  
* **Implementation:**  
  * Calculate H \= ((R & 0xF8) × 3 \+ (G & 0xF8) × 5 \+ (B & 0xF8) × 7 \+ A × 11\) mod 64. *(Masking the bottom 3 bits of RGB ensures perceptually similar colors yield the same hash bucket).*  
  * Let pal\_px \= index\[H\].  
  * If px.a \== pal\_px.a AND the Green-Weighted Manhattan Distance between px and pal\_px is ≤ 6, emit QOI\_OP\_INDEX with payload H.  
  * *Crucial:* Set px \= pal\_px for all subsequent encoding steps (Base Pixel Hash Injection).  
* **Time:** Neutral (+3 cycles for integer bounds check).  
* **Compression:** Massively increased.  
* **Error:** Bounded by E ≤ 6.

**4\. Base Pixel Hash Injection**

* **Mechanism:** The hash array and px\_prev state must only track the *quantized/substituted* values, not the raw source values.  
* **Implementation:** When a pixel is absorbed into a Lossy Run or Lossy Index, the encoder updates index\[H\] and px\_prev using the base/palette pixel's RGB values.  
* **Time:** Neutral.  
* **Compression:** Increased (standardizes the hash array).  
* **Error:** Eliminates decoder mismatch; the decoder only operates on exact bitstreams.

**5\. Repurposed Diff Ranges (QOI\_OP\_DIFF)**

* **Mechanism:** Because values \[-1, 1\] are absorbed by Lossy Runs and Indexing, QOI\_OP\_DIFF is permanently shifted to handle wider disconnected ranges.  
* **Implementation:**  
  * Valid raw differences: {-4, \-3, 2, 3}.  
  * 2-bit mapping (Encoder):  
    * 00 → \-4  
    * 01 → \-3  
    * 10 → 2  
    * 11 → 3  
  * Bitwise implementation: encoded\_val \= (raw\_diff \< 0\) ? (raw\_diff \+ 4\) : (raw\_diff).  
* **Time:** Neutral.  
* **Compression:** Increased (captures medium jumps in 1 byte).  
* **Error:** Introduces quantization since intermediate values (e.g., Δ \= \-2) must be snapped to the nearest valid bin.

**6\. Scaled QOI\_OP\_LUMA**

* **Mechanism:** Bit-shifts the Green channel difference payload to double its reach, capturing high-contrast edges in 2 bytes instead of 4\.  
* **Implementation:**  
  * **Encoder:**  
    * actual\_dg \= px.g \- px\_prev.g  
    * encoded\_dg \= actual\_dg \>\> 1 (Bitwise arithmetic right shift).  
    * Range of encoded\_dg must fall in \[-32, 31\], making the effective capture range of actual\_dg \[-64, 63\]. Store with \+32 bias.  
    * dr\_dg and db\_dg are calculated relative to the *decoded* green difference to prevent cascading error: decoded\_dg \= encoded\_dg \<\< 1.  
  * **Decoder:**  
    * dg \= (payload \- 32\) \<\< 1.  
* **Time:** Neutral (single-cycle bitshift).  
* **Compression:** Greatly increased.  
* **Error:** Introduces a ± 1 quantization error exclusively on the Green channel for large color jumps.

