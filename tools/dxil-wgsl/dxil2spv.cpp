// SPDX-License-Identifier: GPL-2.0-or-later
// dxil2spv <container.dxbc> <out.spv> <out.json>: one DXIL shader to SPIR-V for naga, through dxil-spirv's C API
// (github.com/HansKristian-Work/dxil-spirv, MIT). The binding scheme is the contract with web/gpu/backend.js:
//   CBV b<r> space <s> -> @group(0) @binding(r + 100 s); SRV t -> group 1; sampler s -> group 2; UAV u -> group 3.
// Vertex inputs keep their signature row as location; the JSON lists them by semantic for the input layout.
// D3D's SV_VertexID / SV_InstanceID do not count the base vertex / instance; WebGPU's indices do and WGSL
// has no builtin for the base, so the base is taken as 0 (exact for draws that start at 0).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "dxil_spirv_c.h"

static std::string inputs_json;
static dxil_spv_bool remap_vertex_input(void *, const dxil_spv_d3d_vertex_input *in, dxil_spv_vulkan_vertex_input *out) {
  out->location = in->start_row;
  char b[256]; snprintf(b, sizeof b, "%s{\"semantic\":\"%s\",\"index\":%u,\"location\":%u}", inputs_json.empty() ? "" : ",", in->semantic, in->semantic_index, in->start_row);
  inputs_json += b;
  return DXIL_SPV_TRUE;
}
static std::string res_json;
static void note(const char *kind, const dxil_spv_d3d_binding *b, unsigned set, unsigned binding) {
  char s[256]; snprintf(s, sizeof s, "%s{\"kind\":\"%s\",\"space\":%u,\"register\":%u,\"count\":%u,\"resourceKind\":%u,\"group\":%u,\"binding\":%u}", res_json.empty() ? "" : ",", kind, b->register_space, b->register_index, b->range_size, (unsigned)b->kind, set, binding);
  res_json += s;
}
static void place(dxil_spv_vulkan_binding *v, const dxil_spv_d3d_binding *b, unsigned set) {
  memset(v, 0, sizeof *v); v->set = set; v->binding = b->register_index + 100 * b->register_space;
}
static dxil_spv_bool remap_srv(void *, const dxil_spv_d3d_binding *b, dxil_spv_srv_vulkan_binding *v) {
  memset(v, 0, sizeof *v); place(&v->buffer_binding, b, 1);
  if (b->kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER || b->kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER) v->buffer_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO;
  v->offset_binding.set = 15; note("srv", b, 1, v->buffer_binding.binding); return DXIL_SPV_TRUE;
}
static dxil_spv_bool remap_sampler(void *, const dxil_spv_d3d_binding *b, dxil_spv_vulkan_binding *v) { place(v, b, 2); note("sampler", b, 2, v->binding); return DXIL_SPV_TRUE; }
static dxil_spv_bool remap_uav(void *, const dxil_spv_uav_d3d_binding *b, dxil_spv_uav_vulkan_binding *v) {
  memset(v, 0, sizeof *v); place(&v->buffer_binding, &b->d3d_binding, 3);
  if (b->d3d_binding.kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER || b->d3d_binding.kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER) v->buffer_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO;
  v->counter_binding.set = 14; v->offset_binding.set = 15; note("uav", &b->d3d_binding, 3, v->buffer_binding.binding); return DXIL_SPV_TRUE;
}
static dxil_spv_bool remap_cbv(void *, const dxil_spv_d3d_binding *b, dxil_spv_cbv_vulkan_binding *v) {
  memset(v, 0, sizeof *v); place(&v->vulkan.uniform_binding, b, 0); note("cbv", b, 0, v->vulkan.uniform_binding.binding); return DXIL_SPV_TRUE;
}

// SPIR-V words: the BaseVertex / BaseInstance inputs become private variables that hold 0
// A discard (HLSL clip, texkill) comes out of dxil-spirv as SPV_EXT_demote_to_helper_invocation, which naga does not
// read. Each `OpDemoteToHelperInvocation; OpBranch` (the conditional demote's own block) becomes `OpKill`, and the
// capability and extension go; naga reads OpKill as WGSL's discard, which demotes anyway. False if a demote is used
// otherwise (OpIsHelperInvocationEXT, or not followed by a branch).
static bool kill_for_demote(std::vector<uint32_t> &w) {
  if (w.size() < 5) return true;
  std::vector<uint32_t> out(w.begin(), w.begin() + 5);
  bool ok = true;
  for (size_t i = 5; i < w.size();) {
    uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
    if (!n || i + n > w.size()) return false;
    if (op == 17 && n == 2 && w[i + 1] == 5379) { i += n; continue; } // OpCapability DemoteToHelperInvocation
    if (op == 10 && !strcmp((const char *)&w[i + 1], "SPV_EXT_demote_to_helper_invocation")) { i += n; continue; } // OpExtension
    if (op == 5380) { // OpDemoteToHelperInvocation
      size_t j = i + n;
      if (j < w.size() && (w[j] & 0xffff) == 249) { out.push_back(1u << 16 | 252); i = j + (w[j] >> 16); continue; } // ... OpBranch -> OpKill
      ok = false;
    }
    if (op == 5381) ok = false; // OpIsHelperInvocationEXT
    out.insert(out.end(), w.begin() + i, w.begin() + i + n); i += n;
  }
  w.swap(out);
  return ok;
}
static void drop_draw_parameters(std::vector<uint32_t> &w) {
  enum { OpEntryPoint = 15, OpCapability = 17, OpTypeInt = 21, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71 };
  std::vector<uint32_t> vars;
  for (size_t i = 5; i < w.size(); i += w[i] >> 16)
    if ((w[i] & 0xFFFF) == OpDecorate && (w[i] >> 16) == 4 && w[i + 2] == 11 && (w[i + 3] == 4424 || w[i + 3] == 4425)) vars.push_back(w[i + 1]);
  if (vars.empty()) return;
  uint32_t bound = w[3], uint_t = 0;
  for (size_t i = 5; i < w.size(); i += w[i] >> 16) if ((w[i] & 0xFFFF) == OpTypeInt && w[i + 2] == 32 && w[i + 3] == 0) uint_t = w[i + 1];
  if (!uint_t) return;
  uint32_t ptr = bound++, zero = bound++;
  std::vector<uint32_t> out(w.begin(), w.begin() + 5);
  bool added = false;
  for (size_t i = 5; i < w.size();) {
    uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
    bool keep = true;
    if (op == OpDecorate && n == 4 && w[i + 2] == 11 && (w[i + 3] == 4424 || w[i + 3] == 4425)) keep = false;
    if (op == OpCapability && w[i + 1] == 4427) keep = false; // DrawParameters
    if (op == OpVariable) {
      bool ours = false; for (uint32_t v : vars) ours |= w[i + 2] == v;
      if (ours) {
        if (!added) { out.insert(out.end(), {4u << 16 | OpTypePointer, ptr, 6u /* Private */, uint_t, 4u << 16 | OpConstant, uint_t, zero, 0u}); added = true; }
        out.insert(out.end(), {5u << 16 | OpVariable, ptr, w[i + 2], 6u, zero}); i += n; continue;
      }
    }
    if (keep) out.insert(out.end(), w.begin() + i, w.begin() + i + n);
    i += n;
  }
  out[3] = bound; w.swap(out);
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: dxil2spv <container> <out.spv> <out.json>\n"); return 2; }
  FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
  std::vector<char> bin; char buf[65536]; size_t k; while ((k = fread(buf, 1, sizeof buf, f)) > 0) bin.insert(bin.end(), buf, buf + k); fclose(f);
  dxil_spv_parsed_blob blob;
  if (dxil_spv_parse_dxil_blob(bin.data(), bin.size(), &blob) != DXIL_SPV_SUCCESS) { fprintf(stderr, "dxil2spv: not a DXIL container\n"); return 1; }
  dxil_spv_shader_stage stage = dxil_spv_parsed_blob_get_shader_stage(blob);
  dxil_spv_converter conv;
  if (dxil_spv_create_converter(blob, &conv) != DXIL_SPV_SUCCESS) return 1;
  dxil_spv_converter_set_srv_remapper(conv, remap_srv, nullptr);
  dxil_spv_converter_set_sampler_remapper(conv, remap_sampler, nullptr);
  dxil_spv_converter_set_uav_remapper(conv, remap_uav, nullptr);
  dxil_spv_converter_set_cbv_remapper(conv, remap_cbv, nullptr);
  dxil_spv_converter_set_vertex_input_remapper(conv, remap_vertex_input, nullptr);
  if (dxil_spv_converter_run(conv) != DXIL_SPV_SUCCESS) { fprintf(stderr, "dxil2spv: conversion failed\n"); return 1; }
  dxil_spv_compiled_spirv spv;
  if (dxil_spv_converter_get_compiled_spirv(conv, &spv) != DXIL_SPV_SUCCESS) return 1;
  std::vector<uint32_t> words((const uint32_t *)spv.data, (const uint32_t *)spv.data + spv.size / 4);
  drop_draw_parameters(words);
  if (!kill_for_demote(words)) { fprintf(stderr, "dxil2spv: a demote-to-helper this tool cannot turn into a kill\n"); return 1; }
  FILE *o = fopen(argv[2], "wb"); fwrite(words.data(), 4, words.size(), o); fclose(o);
  const char *stages[] = {"vertex", "hull", "domain", "geometry", "pixel", "compute"};
  unsigned wg[3] = {0, 0, 0};
  if (stage == DXIL_SPV_STAGE_COMPUTE) dxil_spv_converter_get_compute_workgroup_dimensions(conv, &wg[0], &wg[1], &wg[2]);
  FILE *j = fopen(argv[3], "w");
  fprintf(j, "{\"stage\":\"%s\",\"inputs\":[%s],\"resources\":[%s],\"workgroup\":[%u,%u,%u]}\n", (unsigned)stage - 1 < 6 ? stages[stage - 1] : "other", inputs_json.c_str(), res_json.c_str(), wg[0], wg[1], wg[2]);
  fclose(j);
  dxil_spv_converter_free(conv); dxil_spv_parsed_blob_free(blob);
  return 0;
}
