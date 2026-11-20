# AI source-separation model review

Review date: 2026-11-20

## Product requirement

The commercial target is an offline Windows VST3 that can isolate a drum stem from a full mix and, where supported, split that stem into individual kit elements. A model must have a defensible commercial redistribution/use grant, a reproducible inference contract, and acceptable Windows CPU/GPU performance.

## Candidates reviewed

### SCNet ONNX

- Model card: https://huggingface.co/square-zero-labs/scnet
- Four-stem output: drums, bass, other, vocals.
- ONNX int8 export: approximately 12.9 MB; fixed 44.1 kHz stereo segment input.
- The model card records an explicit statement from the weights' author permitting redistribution of the weights and converted ONNX exports under MIT.
- The same card notes training on MUSDB18-HQ, whose dataset licence is educational-use oriented, and says the effect of that dataset licence on the weights has not been settled.

**Decision:** technically promising as the first drum-stem separation stage, but do not bundle it in a commercial release until the dataset/training-rights issue is clarified in writing. The MIT statement for code/weights is not treated as a blanket resolution of training-data rights.

### MDX23C DrumSep 6-stem

- Model/code reference: https://github.com/openmirlab/mdxnet-infer
- Stems: kick, snare, toms, hi-hat, ride, crash.
- The inference code is MIT, but the checkpoint authors did not publish explicit licence terms. The current mirror documents the weights as non-commercial-safe only pending author confirmation.

**Decision:** do not bundle or enable by default in a commercial build.

### inagoy DrumSep ONNX

- Model card: https://huggingface.co/splitzo/drumsep
- Four elements: kick, snare, cymbals, toms.
- The ONNX mirror is tagged MIT, but the original upstream checkpoint's weight licence and training provenance are not sufficiently established by that mirror alone.

**Decision:** do not rely on the mirror's metadata as the sole commercial grant. Obtain upstream confirmation before distribution.

### LarsNet-derived drum separators

- Model card: https://huggingface.co/Faridzar/timbrica-drum-separator
- The referenced weights are CC-BY-NC-SA 4.0 for third parties.

**Decision:** excluded from the commercial product unless a separate commercial licence is obtained.

## Release gate

AI source separation remains a release blocker for the advertised full-mix-to-drum-kit workflow. Before shipping it:

1. Obtain a model with explicit commercial rights covering code, checkpoint weights, and relevant training-data restrictions, or train a replacement model on appropriately licensed data.
2. Pin the exact model revision and SHA-256.
3. Document model inputs, outputs, sample rate, chunking, STFT/iSTFT conventions, and latency/memory requirements.
4. Validate separation quality against a fixed local test corpus, including dense reggaeton/trap/boom-bap mixes.
5. Run offline inference on Windows x64 with CPU and NVIDIA GPU paths, with graceful CPU fallback.
6. Keep model weights separate from the plugin binary if the licence or file size requires first-run installation.

No AI separation is claimed by the current DSP prototype. The existing transient detector only slices the supplied audio; it does not isolate drums from a full mix.
