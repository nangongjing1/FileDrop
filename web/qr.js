/* 极简 QR 码生成器(字节模式 / 纠错等级 M / 版本 1-10)，无外部依赖。 */
(function (global) {
  'use strict';

  const EXP = new Uint8Array(512);
  const LOG = new Uint8Array(256);
  (function buildTables() {
    let x = 1;
    for (let i = 0; i < 255; i += 1) {
      EXP[i] = x;
      LOG[x] = i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11d;
    }
    for (let i = 255; i < 512; i += 1) EXP[i] = EXP[i - 255];
  })();

  function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return EXP[LOG[a] + LOG[b]];
  }

  function generatorPoly(degree) {
    let poly = [1];
    for (let i = 0; i < degree; i += 1) {
      const next = new Array(poly.length + 1).fill(0);
      for (let j = 0; j < poly.length; j += 1) {
        next[j] ^= poly[j];
        next[j + 1] ^= gfMul(poly[j], EXP[i]);
      }
      poly = next;
    }
    return poly;
  }

  function rsRemainder(data, ecLength) {
    const gen = generatorPoly(ecLength);
    const buffer = new Uint8Array(data.length + ecLength);
    buffer.set(data);
    for (let i = 0; i < data.length; i += 1) {
      const factor = buffer[i];
      if (factor === 0) continue;
      for (let j = 0; j < gen.length; j += 1) {
        buffer[i + j] ^= gfMul(gen[j], factor);
      }
    }
    return buffer.slice(data.length);
  }

  // 纠错等级 M：每个版本的 [纠错码字数, [块数, 每块数据码字数]...]
  const VERSIONS = {
    1: { ec: 10, groups: [[1, 16]] },
    2: { ec: 16, groups: [[1, 28]] },
    3: { ec: 26, groups: [[1, 44]] },
    4: { ec: 18, groups: [[2, 32]] },
    5: { ec: 24, groups: [[2, 43]] },
    6: { ec: 16, groups: [[4, 27]] },
    7: { ec: 18, groups: [[4, 31]] },
    8: { ec: 22, groups: [[2, 38], [2, 39]] },
    9: { ec: 22, groups: [[3, 36], [2, 37]] },
    10: { ec: 26, groups: [[4, 43], [1, 44]] },
  };

  const ALIGNMENT = {
    1: [],
    2: [6, 18],
    3: [6, 22],
    4: [6, 26],
    5: [6, 30],
    6: [6, 34],
    7: [6, 22, 38],
    8: [6, 24, 42],
    9: [6, 26, 46],
    10: [6, 28, 50],
  };

  function dataCodewordCount(version) {
    let total = 0;
    VERSIONS[version].groups.forEach((group) => {
      total += group[0] * group[1];
    });
    return total;
  }

  function encodeUtf8(text) {
    if (global.TextEncoder) return Array.from(new TextEncoder().encode(text));
    const bytes = [];
    for (let i = 0; i < text.length; i += 1) {
      const code = text.charCodeAt(i);
      if (code < 0x80) bytes.push(code);
      else if (code < 0x800) bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
      else bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    }
    return bytes;
  }

  function buildCodewords(bytes) {
    let version = 0;
    for (let v = 1; v <= 10; v += 1) {
      const countBits = v >= 10 ? 16 : 8;
      const capacityBits = dataCodewordCount(v) * 8;
      if (4 + countBits + bytes.length * 8 <= capacityBits) {
        version = v;
        break;
      }
    }
    if (!version) throw new Error('内容过长，无法生成 QR 码');

    const countBits = version >= 10 ? 16 : 8;
    const bits = [];
    const push = (value, length) => {
      for (let i = length - 1; i >= 0; i -= 1) bits.push((value >> i) & 1);
    };
    push(0x4, 4);
    push(bytes.length, countBits);
    bytes.forEach((byte) => push(byte, 8));

    const capacityBits = dataCodewordCount(version) * 8;
    const terminator = Math.min(4, capacityBits - bits.length);
    for (let i = 0; i < terminator; i += 1) bits.push(0);
    while (bits.length % 8 !== 0) bits.push(0);

    const data = [];
    for (let i = 0; i < bits.length; i += 8) {
      let byte = 0;
      for (let j = 0; j < 8; j += 1) byte = (byte << 1) | bits[i + j];
      data.push(byte);
    }
    const totalData = dataCodewordCount(version);
    let padToggle = 0;
    while (data.length < totalData) {
      data.push(padToggle === 0 ? 0xec : 0x11);
      padToggle ^= 1;
    }

    const spec = VERSIONS[version];
    const blocks = [];
    let offset = 0;
    spec.groups.forEach((group) => {
      for (let i = 0; i < group[0]; i += 1) {
        const blockData = data.slice(offset, offset + group[1]);
        offset += group[1];
        blocks.push({ data: blockData, ec: rsRemainder(blockData, spec.ec) });
      }
    });

    const maxData = Math.max(...blocks.map((block) => block.data.length));
    const interleaved = [];
    for (let i = 0; i < maxData; i += 1) {
      blocks.forEach((block) => {
        if (i < block.data.length) interleaved.push(block.data[i]);
      });
    }
    for (let i = 0; i < spec.ec; i += 1) {
      blocks.forEach((block) => interleaved.push(block.ec[i]));
    }
    return { version, codewords: interleaved };
  }

  function createMatrix(version) {
    const size = version * 4 + 17;
    const modules = new Uint8Array(size * size);
    const reserved = new Uint8Array(size * size);
    const get = (row, col) => modules[row * size + col];
    const set = (row, col, value) => {
      if (row < 0 || col < 0 || row >= size || col >= size) return;
      modules[row * size + col] = value ? 1 : 0;
      reserved[row * size + col] = 1;
    };

    const finder = (row, col) => {
      for (let r = -1; r <= 7; r += 1) {
        for (let c = -1; c <= 7; c += 1) {
          const rr = row + r;
          const cc = col + c;
          if (rr < 0 || cc < 0 || rr >= size || cc >= size) continue;
          const inner = r >= 0 && r <= 6 && c >= 0 && c <= 6;
          const dark = inner && (r === 0 || r === 6 || c === 0 || c === 6 || (r >= 2 && r <= 4 && c >= 2 && c <= 4));
          set(rr, cc, dark);
        }
      }
    };

    finder(0, 0);
    finder(0, size - 7);
    finder(size - 7, 0);

    for (let i = 8; i < size - 8; i += 1) {
      set(6, i, i % 2 === 0);
      set(i, 6, i % 2 === 0);
    }

    const centers = ALIGNMENT[version] || [];
    centers.forEach((row) => {
      centers.forEach((col) => {
        const nearFinder =
          (row <= 7 && col <= 7) || (row <= 7 && col >= size - 8) || (row >= size - 8 && col <= 7);
        if (nearFinder) return;
        for (let r = -2; r <= 2; r += 1) {
          for (let c = -2; c <= 2; c += 1) {
            const dark = Math.max(Math.abs(r), Math.abs(c)) !== 1;
            set(row + r, col + c, dark);
          }
        }
      });
    });

    // 预留格式信息区
    for (let i = 0; i <= 8; i += 1) {
      if (i !== 6) {
        set(8, i, 0);
        set(i, 8, 0);
      }
    }
    set(8, 8, 0);
    set(size - 8, 8, 1);
    for (let i = 0; i < 8; i += 1) {
      set(size - 1 - i, 8, 0);
      set(8, size - 1 - i, 0);
    }

    if (version >= 7) {
      for (let i = 0; i < 18; i += 1) {
        set(Math.floor(i / 3), size - 11 + (i % 3), 0);
        set(size - 11 + (i % 3), Math.floor(i / 3), 0);
      }
    }

    return { size, modules, reserved, get, set };
  }

  function placeData(matrix, codewords) {
    const { size, get, reserved } = matrix;
    const bits = [];
    codewords.forEach((byte) => {
      for (let i = 7; i >= 0; i -= 1) bits.push((byte >> i) & 1);
    });

    let index = 0;
    let upward = true;
    for (let col = size - 1; col > 0; col -= 2) {
      if (col === 6) col -= 1;
      for (let step = 0; step < size; step += 1) {
        const row = upward ? size - 1 - step : step;
        for (let c = 0; c < 2; c += 1) {
          const targetCol = col - c;
          if (reserved[row * size + targetCol]) continue;
          const bit = index < bits.length ? bits[index] : 0;
          index += 1;
          matrix.modules[row * size + targetCol] = bit;
        }
      }
      upward = !upward;
    }
    return index;
  }

  const MASKS = [
    (row, col) => (row + col) % 2 === 0,
    (row) => row % 2 === 0,
    (row, col) => col % 3 === 0,
    (row, col) => (row + col) % 3 === 0,
    (row, col) => (Math.floor(row / 2) + Math.floor(col / 3)) % 2 === 0,
    (row, col) => ((row * col) % 2) + ((row * col) % 3) === 0,
    (row, col) => (((row * col) % 2) + ((row * col) % 3)) % 2 === 0,
    (row, col) => (((row + col) % 2) + ((row * col) % 3)) % 2 === 0,
  ];

  function formatBits(mask) {
    const data = (0 << 3) | mask; // 纠错等级 M 对应 00
    let value = data << 10;
    let remainder = value;
    for (let i = 14; i >= 10; i -= 1) {
      if ((remainder >> i) & 1) remainder ^= 0x537 << (i - 10);
    }
    return ((data << 10) | remainder) ^ 0x5412;
  }

  function versionBits(version) {
    let value = version << 12;
    let remainder = value;
    for (let i = 17; i >= 12; i -= 1) {
      if ((remainder >> i) & 1) remainder ^= 0x1f25 << (i - 12);
    }
    return value | remainder;
  }

  function applyMask(matrix, maskIndex) {
    const { size, modules, reserved, get } = matrix;
    const masked = new Uint8Array(size * size);
    const mask = MASKS[maskIndex];
    for (let row = 0; row < size; row += 1) {
      for (let col = 0; col < size; col += 1) {
        const value = get(row, col);
        masked[row * size + col] = reserved[row * size + col] ? value : value ^ (mask(row, col) ? 1 : 0);
      }
    }
    return masked;
  }

  function writeFormat(modules, size, mask) {
    const bits = formatBits(mask);
    const bit = (index) => (bits >> index) & 1;

    // 第一份：左上角，沿列 8 向下与沿行 8 向右
    for (let i = 0; i < 15; i += 1) {
      if (i < 6) modules[i * size + 8] = bit(i);
      else if (i < 8) modules[(i + 1) * size + 8] = bit(i);
      else modules[(size - 15 + i) * size + 8] = bit(i);
    }

    // 第二份：左下角与右上角
    for (let i = 0; i < 15; i += 1) {
      if (i < 8) modules[8 * size + (size - 1 - i)] = bit(i);
      else if (i === 8) modules[8 * size + 7] = bit(i);
      else modules[8 * size + (14 - i)] = bit(i);
    }

    modules[(size - 8) * size + 8] = 1;
  }

  function writeVersion(modules, size, version) {
    if (version < 7) return;
    const bits = versionBits(version);
    for (let i = 0; i < 18; i += 1) {
      const value = (bits >> i) & 1;
      const row = Math.floor(i / 3);
      const col = size - 11 + (i % 3);
      modules[row * size + col] = value;
      modules[col * size + row] = value;
    }
  }

  function penalty(modules, size) {
    let score = 0;

    const runPenalty = (getter) => {
      for (let line = 0; line < size; line += 1) {
        let previous = getter(line, 0);
        let run = 1;
        for (let i = 1; i < size; i += 1) {
          const value = getter(line, i);
          if (value === previous) {
            run += 1;
            if (run === 5) score += 3;
            else if (run > 5) score += 1;
          } else {
            run = 1;
            previous = value;
          }
        }
      }
    };

    runPenalty((line, i) => modules[line * size + i]);
    runPenalty((line, i) => modules[i * size + line]);

    for (let row = 0; row < size - 1; row += 1) {
      for (let col = 0; col < size - 1; col += 1) {
        const value = modules[row * size + col];
        if (
          value === modules[row * size + col + 1] &&
          value === modules[(row + 1) * size + col] &&
          value === modules[(row + 1) * size + col + 1]
        ) {
          score += 3;
        }
      }
    }

    const finderPatterns = [
      [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0],
      [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1],
    ];
    const matchAt = (getter) => {
      for (let line = 0; line < size; line += 1) {
        for (let start = 0; start + 11 <= size; start += 1) {
          finderPatterns.forEach((pattern) => {
            let matched = true;
            for (let i = 0; i < 11; i += 1) {
              if (getter(line, start + i) !== pattern[i]) {
                matched = false;
                break;
              }
            }
            if (matched) score += 40;
          });
        }
      }
    };
    matchAt((line, i) => modules[line * size + i]);
    matchAt((line, i) => modules[i * size + line]);

    let dark = 0;
    for (let i = 0; i < modules.length; i += 1) dark += modules[i];
    const ratio = (dark * 100) / (size * size);
    score += Math.floor(Math.abs(ratio - 50) / 5) * 10;

    return score;
  }

  function build(text, forcedMask) {
    const { version, codewords } = buildCodewords(encodeUtf8(text));
    const matrix = createMatrix(version);
    placeData(matrix, codewords);

    let best = null;
    const masks = typeof forcedMask === 'number' ? [forcedMask] : [0, 1, 2, 3, 4, 5, 6, 7];
    masks.forEach((maskIndex) => {
      const modules = applyMask(matrix, maskIndex);
      writeFormat(modules, matrix.size, maskIndex);
      writeVersion(modules, matrix.size, version);
      const score = penalty(modules, matrix.size);
      if (!best || score < best.score) best = { score, modules, mask: maskIndex };
    });

    return {
      version,
      size: matrix.size,
      mask: best.mask,
      modules: best.modules,
    };
  }

  function toSvg(text, options) {
    const opts = options || {};
    const qr = build(text, opts.mask);
    const quiet = typeof opts.quiet === 'number' ? opts.quiet : 4;
    const total = qr.size + quiet * 2;
    const scale = opts.scale || 4;
    const rects = [];
    for (let row = 0; row < qr.size; row += 1) {
      let runStart = -1;
      for (let col = 0; col <= qr.size; col += 1) {
        const dark = col < qr.size && qr.modules[row * qr.size + col] === 1;
        if (dark && runStart < 0) runStart = col;
        if (!dark && runStart >= 0) {
          rects.push(
            '<rect x="' + (runStart + quiet) * scale + '" y="' + (row + quiet) * scale +
              '" width="' + (col - runStart) * scale + '" height="' + scale + '"/>'
          );
          runStart = -1;
        }
      }
    }
    const dimension = total * scale;
    return (
      '<svg xmlns="http://www.w3.org/2000/svg" width="' + dimension + '" height="' + dimension +
      '" viewBox="0 0 ' + dimension + ' ' + dimension + '" shape-rendering="crispEdges">' +
      '<rect width="100%" height="100%" fill="#ffffff"/>' +
      '<g fill="#000000">' + rects.join('') + '</g></svg>'
    );
  }

  global.FiledropQR = { build, toSvg };
})(window);
