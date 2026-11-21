// Prototype: the far-field fine bake (farfield_bake.comp, ray queries on the
// BLAS) computed on the CPU from the triangles. Same ray grid: per voxel,
// R x R rays per axis through the voxel's faces, each ray's nearest hit inside
// the voxel's own segment. Rays are produced by rasterizing every triangle
// into the 2D ray grid of each axis (triangles binned by voxel row), keeping
// the nearest hit per (ray, voxel). Alpha is 1 (no texture decode).
//
// usage: meridian-cpuvox model.gltf [threads] [bruteRays]
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kRays = 4; // kProtoBakeRays
constexpr uint32_t kVoxelsPerHeight = 128;
constexpr float kInf = std::numeric_limits<float>::infinity();

struct Tri {
  float p[3][3];
  float n[3][3];
  uint32_t slot;
};

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct Grid {
  float origin[3];
  float voxel;
  uint32_t size[3];
  size_t count() const { return size_t{size[0]} * size[1] * size[2]; }
  size_t index(uint32_t c[3]) const { return c[0] + size_t{size[0]} * (c[1] + size_t{size[1]} * c[2]); }
};

struct AxisOut {
  std::vector<uint8_t> hits;
  std::vector<std::array<float, 3>> normal;
  std::vector<std::array<uint8_t, 4>> slot;
  std::atomic<uint64_t> covered{0};
  std::vector<uint8_t> rayHit;
};

struct Hit {
  float t = kInf;
  uint32_t tri = 0;
  float w1 = 0, w2 = 0;
};

void runRow(const Grid &g, const std::vector<Tri> &tris, const std::vector<uint32_t> &ids, int ax,
            uint32_t ui, AxisOut &out, std::vector<Hit> &best) {
  const int ui_axis = (ax + 2) % 3, wi_axis = (ax + 1) % 3;
  const uint32_t nk = g.size[ax], nb = g.size[wi_axis] * kRays;
  const float step = g.voxel / kRays;
  best.assign(size_t{kRays} * nb * nk, Hit{});
  for (uint32_t id : ids) {
    const Tri &t = tris[id];
    float umin = 1e30f, umax = -1e30f, wmin = 1e30f, wmax = -1e30f;
    for (int v = 0; v < 3; ++v) {
      umin = std::min(umin, t.p[v][ui_axis]);
      umax = std::max(umax, t.p[v][ui_axis]);
      wmin = std::min(wmin, t.p[v][wi_axis]);
      wmax = std::max(wmax, t.p[v][wi_axis]);
    }
    const float ou = g.origin[ui_axis] + ui * g.voxel, ow = g.origin[wi_axis];
    const int a0 = std::max(0, static_cast<int>(std::ceil((umin - ou) / step - 0.5f)));
    const int a1 = std::min<int>(kRays - 1, static_cast<int>(std::floor((umax - ou) / step - 0.5f)));
    const int b0 = std::max(0, static_cast<int>(std::ceil((wmin - ow) / step - 0.5f)));
    const int b1 = std::min<int>(nb - 1, static_cast<int>(std::floor((wmax - ow) / step - 0.5f)));
    if (a0 > a1 || b0 > b1) {
      continue;
    }
    const double ax0 = t.p[0][ui_axis], ay0 = t.p[0][wi_axis];
    const double e1x = t.p[1][ui_axis] - ax0, e1y = t.p[1][wi_axis] - ay0;
    const double e2x = t.p[2][ui_axis] - ax0, e2y = t.p[2][wi_axis] - ay0;
    const double det = e1x * e2y - e2x * e1y;
    if (det == 0.0) {
      continue;
    }
    const double inv = 1.0 / det;
    for (int a = a0; a <= a1; ++a) {
      const double pu = ou + (a + 0.5) * step - ax0;
      for (int b = b0; b <= b1; ++b) {
        const double pw = ow + (b + 0.5) * step - ay0;
        const double w1 = (pu * e2y - e2x * pw) * inv;
        const double w2 = (e1x * pw - pu * e1y) * inv;
        if (w1 < 0.0 || w2 < 0.0 || w1 + w2 > 1.0) {
          continue;
        }
        const double z = t.p[0][ax] + w1 * (t.p[1][ax] - t.p[0][ax]) + w2 * (t.p[2][ax] - t.p[0][ax]);
        const double rel = (z - g.origin[ax]) / g.voxel;
        if (rel < 0.0 || rel > nk) {
          continue;
        }
        const uint32_t k = std::min<uint32_t>(static_cast<uint32_t>(rel), nk - 1);
        Hit &h = best[(size_t{static_cast<uint32_t>(a)} * nb + b) * nk + k];
        if (static_cast<float>(z) < h.t) {
          h = {static_cast<float>(z), id, static_cast<float>(w1), static_cast<float>(w2)};
        }
      }
    }
  }
  uint64_t covered = 0;
  for (uint32_t a = 0; a < kRays; ++a) {
    for (uint32_t b = 0; b < nb; ++b) {
      bool any = false;
      for (uint32_t k = 0; k < nk; ++k) {
        const Hit &h = best[(size_t{a} * nb + b) * nk + k];
        if (h.t == kInf) {
          continue;
        }
        any = true;
        uint32_t c[3];
        c[ax] = k;
        c[ui_axis] = ui;
        c[wi_axis] = b / kRays;
        const size_t vi = g.index(c);
        const Tri &t = tris[h.tri];
        const float w0 = 1.0f - h.w1 - h.w2;
        float n[3], len = 0;
        for (int d = 0; d < 3; ++d) {
          n[d] = w0 * t.n[0][d] + h.w1 * t.n[1][d] + h.w2 * t.n[2][d];
          len += n[d] * n[d];
        }
        len = std::sqrt(len);
        if (len > 0) {
          for (int d = 0; d < 3; ++d) {
            out.normal[vi][d] += n[d] / len;
          }
        }
        ++out.hits[vi];
        ++out.slot[vi][std::min<uint32_t>(t.slot, 3)];
      }
      covered += any;
      out.rayHit[(size_t{ui} * kRays + a) * nb + b] = any;
    }
  }
  out.covered += covered;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    return 2;
  }
  const unsigned threads = argc > 2 ? std::atoi(argv[2]) : 1;
  const int bruteRays = argc > 3 ? std::atoi(argv[3]) : 0;

  auto t0 = Clock::now();
  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission);
  auto data = fastgltf::GltfDataBuffer::FromPath(argv[1]);
  auto loaded = parser.loadGltf(data.get(), std::filesystem::path(argv[1]).parent_path(),
                                fastgltf::Options::LoadExternalBuffers);
  if (loaded.error() != fastgltf::Error::None) {
    std::fprintf(stderr, "load failed\n");
    return 1;
  }
  const fastgltf::Asset &asset = loaded.get();
  std::vector<Tri> tris;
  uint32_t slot = 0;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto &node : asset.nodes) { // node transforms are identity in the cooked trees
    if (!node.meshIndex) {
      continue;
    }
    for (const auto &prim : asset.meshes[*node.meshIndex].primitives) {
      std::vector<std::array<float, 3>> pos, nor;
      const auto &pa = asset.accessors[prim.findAttribute("POSITION")->accessorIndex];
      pos.resize(pa.count);
      nor.assign(pa.count, {0, 1, 0});
      fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
          asset, pa, [&](fastgltf::math::fvec3 p, size_t i) { pos[i] = {p.x(), p.y(), p.z()}; });
      if (auto it = prim.findAttribute("NORMAL"); it != prim.attributes.cend()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            asset, asset.accessors[it->accessorIndex],
            [&](fastgltf::math::fvec3 n, size_t i) { nor[i] = {n.x(), n.y(), n.z()}; });
      }
      std::vector<uint32_t> idx(asset.accessors[*prim.indicesAccessor].count);
      fastgltf::copyFromAccessor<uint32_t>(asset, asset.accessors[*prim.indicesAccessor], idx.data());
      for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        Tri t;
        t.slot = slot;
        for (int v = 0; v < 3; ++v) {
          for (int d = 0; d < 3; ++d) {
            t.p[v][d] = pos[idx[i + v]][d];
            t.n[v][d] = nor[idx[i + v]][d];
            lo[d] = std::min(lo[d], t.p[v][d]);
            hi[d] = std::max(hi[d], t.p[v][d]);
          }
        }
        tris.push_back(t);
      }
      ++slot;
    }
  }
  const double loadMs = msSince(t0);

  Grid g;
  g.voxel = (hi[1] - lo[1]) / kVoxelsPerHeight;
  for (int d = 0; d < 3; ++d) {
    g.origin[d] = lo[d];
    g.size[d] = static_cast<uint32_t>(std::ceil((hi[d] - lo[d]) / g.voxel)) + 1;
  }
  std::printf("%s: %zu tris, %u slots, voxel %.3f m, grid %ux%ux%u (%.2fM)\n", argv[1], tris.size(),
              slot, g.voxel, g.size[0], g.size[1], g.size[2], g.count() / 1e6);

  t0 = Clock::now();
  // rows[ax][ui] -> triangle ids, CSR.
  std::array<std::vector<uint32_t>, 3> start, list;
  for (int ax = 0; ax < 3; ++ax) {
    const int u = (ax + 2) % 3;
    const uint32_t nu = g.size[u];
    start[ax].assign(nu + 1, 0);
    auto range = [&](const Tri &t, uint32_t &r0, uint32_t &r1) {
      float mn = 1e30f, mx = -1e30f;
      for (int v = 0; v < 3; ++v) {
        mn = std::min(mn, t.p[v][u]);
        mx = std::max(mx, t.p[v][u]);
      }
      // A ray at row r has u in [r, r+1) voxels; rows overlapping [mn, mx].
      r0 = static_cast<uint32_t>(std::max(0.0f, std::floor((mn - g.origin[u]) / g.voxel)));
      const float top = std::floor((mx - g.origin[u]) / g.voxel);
      r1 = static_cast<uint32_t>(std::min<float>(nu - 1, std::max(top, 0.0f)));
    };
    for (const Tri &t : tris) {
      uint32_t r0, r1;
      range(t, r0, r1);
      for (uint32_t r = r0; r <= r1; ++r) {
        ++start[ax][r + 1];
      }
    }
    for (uint32_t r = 0; r < nu; ++r) {
      start[ax][r + 1] += start[ax][r];
    }
    list[ax].resize(start[ax][nu]);
    std::vector<uint32_t> fill(start[ax].begin(), start[ax].end() - 1);
    for (uint32_t i = 0; i < tris.size(); ++i) {
      uint32_t r0, r1;
      range(tris[i], r0, r1);
      for (uint32_t r = r0; r <= r1; ++r) {
        list[ax][fill[r]++] = i;
      }
    }
  }
  const double binMs = msSince(t0);

  t0 = Clock::now();
  std::array<AxisOut, 3> out;
  for (auto &o : out) {
    o.hits.assign(g.count(), 0);
    o.normal.assign(g.count(), {0, 0, 0});
    o.slot.assign(g.count(), {0, 0, 0, 0});
  }
  for (int ax = 0; ax < 3; ++ax) {
    out[ax].rayHit.assign(size_t{g.size[(ax + 2) % 3]} * kRays * g.size[(ax + 1) % 3] * kRays, 0);
  }
  std::atomic<uint32_t> next{0};
  const uint32_t tasks = g.size[0] + g.size[1] + g.size[2];
  auto worker = [&] {
    std::vector<Hit> best;
    for (uint32_t i; (i = next++) < tasks;) {
      int ax = 0;
      uint32_t row = i;
      // Task i -> (axis, row of that axis's u dimension).
      const uint32_t nu[3] = {g.size[2], g.size[0], g.size[1]};
      while (row >= nu[ax]) {
        row -= nu[ax++];
      }
      runRow(g, tris, std::vector<uint32_t>(list[ax].begin() + start[ax][row],
                                            list[ax].begin() + start[ax][row + 1]),
             ax, row, out[ax], best);
    }
  };
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < threads; ++i) {
    pool.emplace_back(worker);
  }
  worker();
  for (auto &t : pool) {
    t.join();
  }
  const double rasterMs = msSince(t0);
  std::printf("load %.0f ms (glTF parse, not part of the cook cost), bin %.0f ms, raster %.0f ms "
              "(%u threads)\n",
              loadMs, binMs, rasterMs, threads);

  // Resolve to the fine grid.
  constexpr uint32_t kTotal = 3 * kRays * kRays;
  size_t occupied = 0;
  double extSum = 0;
  std::vector<size_t> brickOcc;
  const uint32_t bs[3] = {(g.size[0] + 3) / 4, (g.size[1] + 3) / 4, (g.size[2] + 3) / 4};
  std::vector<uint8_t> brickFlag(size_t{bs[0]} * bs[1] * bs[2], 0);
  std::vector<uint32_t> packed; // mock 8 B voxel, brick ordered, occupied bricks only
  std::vector<float> ext(g.count(), 0.0f);
  std::vector<std::array<float, 3>> nrm(g.count());
  std::vector<uint8_t> alb(g.count(), 0);
  for (uint32_t z = 0; z < g.size[2]; ++z)
    for (uint32_t y = 0; y < g.size[1]; ++y)
      for (uint32_t x = 0; x < g.size[0]; ++x) {
        uint32_t c[3] = {x, y, z};
        const size_t i = g.index(c);
        uint32_t hits = 0;
        std::array<float, 3> n{};
        uint32_t sl[4] = {};
        for (auto &o : out) {
          hits += o.hits[i];
          for (int d = 0; d < 3; ++d) n[d] += o.normal[i][d];
          for (int s = 0; s < 4; ++s) sl[s] += o.slot[i][s];
        }
        if (hits == 0) continue;
        const uint32_t passes = kTotal - hits;
        const double pass = std::max<double>(passes, 0.5) / kTotal;
        ext[i] = static_cast<float>(-std::log(pass) / g.voxel);
        nrm[i] = n;
        alb[i] = static_cast<uint8_t>(std::max_element(sl, sl + 4) - sl);
        ++occupied;
        extSum += ext[i];
        brickFlag[(x / 4) + size_t{bs[0]} * ((y / 4) + size_t{bs[1]} * (z / 4))] = 1;
      }
  size_t bricks = 0;
  for (uint32_t bz = 0; bz < bs[2]; ++bz)
    for (uint32_t by = 0; by < bs[1]; ++by)
      for (uint32_t bx = 0; bx < bs[0]; ++bx) {
        if (!brickFlag[bx + size_t{bs[0]} * (by + size_t{bs[1]} * bz)]) continue;
        ++bricks;
        for (uint32_t k = 0; k < 64; ++k) {
          uint32_t c[3] = {bx * 4 + (k & 3), by * 4 + ((k >> 2) & 3), bz * 4 + (k >> 4)};
          uint32_t w0 = 0, w1 = 0;
          if (c[0] < g.size[0] && c[1] < g.size[1] && c[2] < g.size[2]) {
            const size_t i = g.index(c);
            if (ext[i] > 0) {
              const float len = std::sqrt(nrm[i][0] * nrm[i][0] + nrm[i][1] * nrm[i][1] +
                                          nrm[i][2] * nrm[i][2]);
              auto q = [&](float v) { return static_cast<uint32_t>((v / (len + 1e-9f) * 0.5f + 0.5f) * 255.f); };
              w0 = std::min<uint32_t>(255, static_cast<uint32_t>(ext[i] * g.voxel * 255.f)) |
                   q(nrm[i][0]) << 8 | q(nrm[i][1]) << 16 | q(nrm[i][2]) << 24;
              w1 = alb[i] | (255u << 8);
            }
          }
          packed.push_back(w0);
          packed.push_back(w1);
        }
      }
  const size_t rawBytes = packed.size() * 4;
  std::vector<char> comp(ZSTD_compressBound(rawBytes));
  const size_t compBytes = ZSTD_compress(comp.data(), comp.size(), packed.data(), rawBytes, 9);
  std::printf("occupied voxels %zu (%.1f%% of grid), mean extinction %.2f /m, occupied bricks %zu "
              "(%.1f of 64 voxels)\n",
              occupied, 100.0 * occupied / g.count(), extSum / std::max<size_t>(occupied, 1), bricks,
              bricks ? double(occupied) / bricks : 0.0);
  std::printf("cache level 0: raw %.2f MiB, zstd-9 %.2f MiB (x1.14 for mips: %.2f / %.2f MiB)\n",
              rawBytes / 1048576.0, compBytes / 1048576.0, rawBytes * 1.14 / 1048576.0,
              compBytes * 1.14 / 1048576.0);
  for (int ax = 0; ax < 3; ++ax) {
    const uint32_t u = (ax + 2) % 3, w = (ax + 1) % 3;
    std::printf("axis %d projected coverage %.4f\n", ax,
                double(out[ax].covered) / (double(g.size[u]) * kRays * g.size[w] * kRays));
  }

  if (bruteRays > 0) {
    // Moller-Trumbore on random rays of the same grid against every triangle.
    std::mt19937 rng(7);
    int mismatch = 0, total = 0;
    for (int ax = 0; ax < 3; ++ax) {
      const int u = (ax + 2) % 3, w = (ax + 1) % 3;
      for (int s = 0; s < bruteRays; ++s) {
        const uint32_t a = rng() % (g.size[u] * kRays), b = rng() % (g.size[w] * kRays);
        const double pu = g.origin[u] + (a + 0.5) * (g.voxel / kRays);
        const double pw = g.origin[w] + (b + 0.5) * (g.voxel / kRays);
        bool hit = false;
        for (const Tri &t : tris) {
          const double e1x = t.p[1][u] - t.p[0][u], e1y = t.p[1][w] - t.p[0][w];
          const double e2x = t.p[2][u] - t.p[0][u], e2y = t.p[2][w] - t.p[0][w];
          const double det = e1x * e2y - e2x * e1y;
          if (det == 0) continue;
          const double qx = pu - t.p[0][u], qy = pw - t.p[0][w];
          const double w1 = (qx * e2y - e2x * qy) / det, w2 = (e1x * qy - qx * e1y) / det;
          if (w1 >= 0 && w2 >= 0 && w1 + w2 <= 1) {
            const double z = t.p[0][ax] + w1 * (t.p[1][ax] - t.p[0][ax]) + w2 * (t.p[2][ax] - t.p[0][ax]);
            if (z >= g.origin[ax] && z <= g.origin[ax] + g.size[ax] * g.voxel) { hit = true; break; }
          }
        }
        ++total;
        mismatch += hit != static_cast<bool>(out[ax].rayHit[size_t{a} * g.size[w] * kRays + b]);
      }
    }
    std::printf("brute force check: %d rays, %d hit/miss mismatches\n", total, mismatch);
  }
  return 0;
}
