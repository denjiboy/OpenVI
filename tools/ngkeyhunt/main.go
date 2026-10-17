// ngkeyhunt - find AES-256 keys that decrypt an RPF7 table of contents.
//
// The GTA V "NG" archives (tag 0x0FEFFFFF, used by PC Legacy and PC Enhanced)
// encrypt the table of contents with a key taken from a key table.  Neither the
// game nor OpenIV ships that table as a file, so the key material lives as
// constant data inside an executable.
//
// The first table entry of an RPF7 archive is always the root directory, whose
// plaintext is known: nameOffset = 0, compressedSize = 0, offsetBlocks =
// 0x7FFFFF, isResource = 0, which in the little endian entry layout is
//   00 00 00 00 00 FF FF 7F   <<-- d1 = offsetBlocks << 8
// That signature is 64 bits wide, so it identifies a real key with effectively
// no false positives.  (It also holds for CBC mode with a zero IV, because the
// first block is then simply AES_dec(C0).)
//
// usage: ngkeyhunt <blob-with-key-material> <archive.rpf>
package main

import (
	"crypto/aes"
	"encoding/binary"
	"fmt"
	"os"
)

var wantLE = [8]byte{0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F}
var wantBE = [8]byte{0x7F, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00}

func matches(buf []byte) (string, bool) {
	if string(buf[:8]) == string(wantLE[:]) {
		return "little endian layout", true
	}
	if string(buf[:8]) == string(wantBE[:]) {
		return "big endian layout", true
	}
	return "", false
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage: %s <blob> <archive.rpf>\n", os.Args[0])
		os.Exit(2)
	}

	blob, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	f, err := os.Open(os.Args[2])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer f.Close()
	hdr := make([]byte, 32)
	if _, err := f.Read(hdr); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	le := binary.LittleEndian.Uint32(hdr) == 0x52504637
	rd := binary.LittleEndian.Uint32
	if !le {
		rd = binary.BigEndian.Uint32
	}
	entryCount := rd(hdr[4:])
	namesField := rd(hdr[8:])
	tag := rd(hdr[12:])
	ct := hdr[16:32] // first entry of the encrypted table

	fmt.Printf("archive  %s\n", os.Args[2])
	fmt.Printf("  entries  %d\n  names    %d\n  tag      0x%08X\n  le       %v\n",
		entryCount, namesField&0x0FFFFFFF, tag, le)
	fmt.Printf("  ciphertext block 0: % X\n", ct)
	fmt.Printf("blob     %s (%d bytes, %d candidate offsets)\n", os.Args[1], len(blob), len(blob)-31)

	keyLens := []int{32, 24, 16}
	if os.Getenv("NGHUNT_NOFILTER") != "" {
		fmt.Println("entropy filter disabled")
	}
	noFilter := os.Getenv("NGHUNT_NOFILTER") != ""
	hits := 0
	var buf [16]byte
	var cnt [256]uint16
	var maxf uint16
	var distinct int
	for i := 0; i+32 <= len(blob); i++ {
		if !noFilter {
			// cheap entropy filter: real keys look like random 32 byte strings
			distinct = 0
			for j := range cnt {
				cnt[j] = 0
			}
			for j := 0; j < 32; j++ {
				if cnt[blob[i+j]] == 0 {
					distinct++
				}
				cnt[blob[i+j]]++
			}
			maxf = 0
			for j := 0; j < 256; j++ {
				if cnt[j] > maxf {
					maxf = cnt[j]
				}
			}
			if distinct < 20 || maxf > 4 {
				continue
			}
		}

		for _, kl := range keyLens {
			block, err := aes.NewCipher(blob[i : i+kl])
			if err != nil {
				continue
			}
			block.Decrypt(buf[:], ct)
			if layout, ok := matches(buf[:]); ok {
				fmt.Printf("\n*** HIT at offset 0x%X *** (%s, AES-%d)\n  key % X\n  d2(first child) = 0x%08X  d3(children) = 0x%08X\n",
					i, layout, kl*8, blob[i:i+kl], rd(buf[8:]), rd(buf[12:]))
				hits++
			}
		}
	}
	fmt.Printf("\n%d hit(s)\n", hits)
}
