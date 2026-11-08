# 모델 파일의 출처와 라이선스

한글 OCR과 텍스트 분류·학습에 쓰는 ONNX 모델은 합쳐서 230MB쯤 되기 때문에 저장소에는 넣지 않고 GitHub Releases에 `models-<버전>.zip`으로 따로 올립니다. 이 프로젝트의 GPLv2는 코드에만 적용되고, 모델 가중치에는 각자의 원래 라이선스가 그대로 적용됩니다.

아래 내용은 2026년 11월에 각 모델 페이지를 직접 확인한 것입니다. 세 모델 모두 재배포와 상업적 사용이 가능하고, 출처 표시 같은 조건만 지키면 됩니다.

| 폴더 | 파일 | 원본 | 라이선스 | 지킬 조건 |
|---|---|---|---|---|
| `models/kor_unsmile/` | `model_int8.onnx`, vocab, labels, rules | [smilegate-ai/kor_unsmile](https://huggingface.co/smilegate-ai/kor_unsmile) | Apache-2.0 | 출처와 NOTICE 표시 |
| `models/kor_embed/` | `kosimcse_embed_int8.onnx`, vocab | [BM-K/KoSimCSE-bert](https://huggingface.co/BM-K/KoSimCSE-bert) | CC BY-SA 4.0 | 출처 표시, 고쳐서 배포하면 같은 라이선스 |
| `models/ocr/` | `det.onnx`, `rec_korean_v5.onnx`, `cls.onnx`, `korean_dict_v5.txt` | [PaddlePaddle/korean_PP-OCRv5_mobile_rec](https://huggingface.co/PaddlePaddle/korean_PP-OCRv5_mobile_rec) 등 | Apache-2.0 | 출처와 NOTICE 표시 |

## 모델별 설명

kor_unsmile은 Smilegate AI가 공개한 혐오 표현 분류 모델로, KcBERT를 파인튜닝한 것입니다. Smilegate는 저장소에서 소스 코드와 기본 모델은 Apache 2.0, 데이터셋은 CC-BY-NC-ND 4.0이라고 밝히고 있습니다. 여기에는 ONNX로 내보낸 모델 가중치만 들어 있고 데이터셋은 들어 있지 않으므로 Apache-2.0을 따릅니다. Hugging Face 모델 카드에는 라이선스 표시가 비어 있을 수 있는데, 위의 저장소 고지를 근거로 삼았습니다.

KoSimCSE-bert는 BM-K의 Korean-Sentence-Embedding 저장소에서 나온 문장 임베딩 모델이고 CC BY-SA 4.0입니다. 쓰려면 출처(BM-K/KoSimCSE-bert)를 밝혀야 하고, 이 모델을 파인튜닝하는 식으로 고쳐서 배포하면 그 결과물도 CC BY-SA 4.0으로 공개해야 합니다. 형식만 ONNX로 바꾼 파일을 그대로 다시 배포하는 것은 문제가 없습니다. 코드와는 라이선스가 따로라서 GPLv2와 부딪히지 않습니다.

PP-OCRv5 한국어 모델은 PaddlePaddle의 모델 카드에 `license: apache-2.0`으로 적혀 있고, PaddleOCR 프로젝트 전체가 Apache-2.0입니다. 검출, 인식, 방향 분류 모델 모두 재배포와 상업적 사용이 가능합니다.

## 출처 표시 문구

릴리스 본문이나 NOTICE 파일에 아래 문구를 넣습니다.

```
This release bundles third-party model weights under their own licenses:
- kor_unsmile (Smilegate AI), Apache-2.0: https://huggingface.co/smilegate-ai/kor_unsmile
- KoSimCSE-bert (BM-K), CC BY-SA 4.0: https://huggingface.co/BM-K/KoSimCSE-bert
- PP-OCRv5 Korean (PaddlePaddle), Apache-2.0: https://huggingface.co/PaddlePaddle/korean_PP-OCRv5_mobile_rec
See MODELS.md for terms. The project's own code is GPLv2 and separate from these weights.
```

## 직접 만들기

원래 플러그인 저장소에 있는 export 스크립트로 같은 모델을 만들 수 있습니다.

```bash
pip install torch transformers onnx onnxruntime paddleocr
python export_kor_unsmile.py   # models/kor_unsmile/ (smilegate-ai/kor_unsmile)
python export_kosimcse.py      # models/kor_embed/   (BM-K/KoSimCSE-bert)
# OCR은 PaddleOCR 한국어 det/rec/cls 모델을 받아 ONNX로 바꾼 뒤 models/ocr/에 둡니다.
```

## 모델 없이 쓰는 경우

모델이 없어도 엔진은 정상으로 돌아가고, 딥러닝을 쓰는 함수만 빈 값을 돌려줍니다. `TextClsAvailable()`과 `TextNNAvailable()`은 0을, `OcrImageAI()`는 빈 문자열을 돌려줍니다. Windows에 들어 있는 OCR을 쓰는 `OcrImage()`는 모델 없이도 동작합니다. `onnxruntime.dll`은 필요할 때 불러오므로 엔진과 같은 폴더에 있어야 하고, 릴리스의 엔진 묶음에 들어 있습니다.

라이선스는 바뀔 수 있으니, 다시 배포하기 전에 각 모델 페이지를 한 번 더 확인해 주세요.
