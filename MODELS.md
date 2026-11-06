# 번들 모델 출처 · 라이선스

딥러닝 기능(한글 OCR, 텍스트 분류/학습)에 쓰는 ONNX 모델은 용량이 커서(~230MB)
**리포에 포함하지 않고 GitHub Releases(`models-<ver>.zip`)로 배포**합니다.
이 프로젝트의 GPLv2는 **코드에만** 적용되고, 모델 가중치에는 각 원본 라이선스가 적용됩니다.

아래는 2026-11 기준으로 각 모델 페이지의 라이선스를 **직접 확인한** 결과입니다.
세 모델 모두 **재배포·상업적 사용이 가능**하며, 각 조건(출처 표시 등)을 지키면 됩니다.

| 폴더 | 모델 | 원본 | 라이선스 | 재배포 | 조건 |
|---|---|---|---|---|---|
| `models/kor_unsmile/` | `model_int8.onnx` + vocab/labels/rules | [smilegate-ai/kor_unsmile](https://huggingface.co/smilegate-ai/kor_unsmile) (KcBERT 파인튜닝) | **Apache-2.0** (모델) | ✅ 상업적 사용 가능 | 출처·NOTICE 표시 |
| `models/kor_embed/` | `kosimcse_embed_int8.onnx` + vocab | [BM-K/KoSimCSE-bert](https://huggingface.co/BM-K/KoSimCSE-bert) | **CC BY-SA 4.0** | ✅ 상업적 사용 가능 | 출처 표시 + **동일조건 변경허락(ShareAlike)** |
| `models/ocr/` | `det.onnx`, `rec_korean_v5.onnx`, `cls.onnx`, `korean_dict_v5.txt` | [PaddlePaddle/korean_PP-OCRv5_mobile_rec](https://huggingface.co/PaddlePaddle/korean_PP-OCRv5_mobile_rec) 외 | **Apache-2.0** | ✅ 상업적 사용 가능 | 출처·NOTICE 표시 |

## 라이선스 상세 (직접 확인)

### kor_unsmile — Apache-2.0 (모델), 데이터셋은 별개
Smilegate AI의 공식 고지: **"UnSmile의 소스코드 및 baseline 모델은 Apache 2.0, 데이터셋은
CC-BY-NC-ND 4.0"**. 우리는 **export된 모델 가중치만** 번들하므로 Apache-2.0가 적용되어
상업적 재배포가 가능합니다. **데이터셋(비상업 NC-ND)은 포함하지 않습니다.**

### KoSimCSE — CC BY-SA 4.0 (ShareAlike 주의)
BM-K의 Korean-Sentence-Embedding 저장소 라이선스: **CC BY-SA 4.0**.
- **BY(저작자표시):** 출처(BM-K/KoSimCSE-bert)를 명시해야 합니다.
- **SA(동일조건변경허락):** 이 모델을 **개조(파인튜닝 등)해 배포**하면 그 결과물도
  CC BY-SA 4.0으로 공개해야 합니다. 단순히 포맷만 바꾼 ONNX 변환본을 **그대로 재배포**하는
  것은 괜찮습니다. (코드와는 별개 라이선스이므로 GPLv2와 충돌하지 않습니다.)

### PaddleOCR 한글 (PP-OCRv5) — Apache-2.0
`PaddlePaddle/korean_PP-OCRv5_mobile_rec` 모델카드에 `license: apache-2.0` 명시.
PaddleOCR 프로젝트 전체가 Apache-2.0. det/rec/cls 모델 모두 재배포·상업적 사용 가능.

## 출처 표시(ATTRIBUTION) — Releases 본문/NOTICE에 넣을 문구
```
This release bundles third-party model weights under their own licenses:
- kor_unsmile (Smilegate AI) — Apache-2.0 — https://huggingface.co/smilegate-ai/kor_unsmile
- KoSimCSE-bert (BM-K)       — CC BY-SA 4.0 — https://huggingface.co/BM-K/KoSimCSE-bert
- PP-OCRv5 Korean (PaddlePaddle) — Apache-2.0 — https://huggingface.co/PaddlePaddle/korean_PP-OCRv5_mobile_rec
See MODELS.md for terms. The project's own code is GPLv2 and separate from these weights.
```

## 직접 만들기 (재현)
원본 플러그인 저장소의 export 스크립트로 동일 모델을 생성할 수 있습니다.
```bash
pip install torch transformers onnx onnxruntime paddleocr
python export_kor_unsmile.py   # -> models/kor_unsmile/   (smilegate-ai/kor_unsmile)
python export_kosimcse.py      # -> models/kor_embed/      (BM-K/KoSimCSE-bert)
# OCR: PaddleOCR 한국어 det/rec/cls 모델을 받아 ONNX 로 변환해 models/ocr/ 에 배치
```

## 모델 없이 쓰기
모델이 없어도 엔진은 정상 동작하며, 딥러닝/AI OCR 계열 함수만 빈 값을 돌려줍니다.
- `TextClsAvailable()` / `TextNNAvailable()` → 0
- `OcrImageAI()` → 빈 문자열 (Windows 내장 `OcrImage()`는 모델 없이 동작)

`onnxruntime.dll`은 엔진 옆에 있어야 합니다(지연 로드). 이 역시 Releases의 엔진 묶음에 포함됩니다.

> 위 라이선스는 2026-11 확인 기준입니다. 재배포 전 각 모델 페이지에서 변경 여부를 한 번 더
> 확인하세요. kor_unsmile 모델카드에는 라이선스 태그가 비어 있을 수 있으나, Smilegate의
> 공식 저장소 고지(소스·모델 Apache-2.0)를 근거로 합니다.
