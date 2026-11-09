# Modelo de visão do ILrDOS

`face_detection_yunet_2023mar.onnx` é o detector facial YuNet do OpenCV Zoo, importado de `glados-feira/src/glados/vision/models/` do ZIP enviado.

- Origem: https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet
- SHA-256: `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`
- Tamanho: 232.589 bytes
- Licença MIT: ver `LICENSE`

O modelo detecta rostos, não identifica pessoas. O ILrDOS processa uma imagem da câmera por solicitação e não salva a imagem.

`silero_vad_16k_op15.onnx` é o detector de atividade de voz fornecido pelo usuário e usado pela ponte de voz do Pi.

- SHA-256: `7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49`
- Taxa de amostragem: 16 kHz; janela de inferência: 512 amostras.
- O serviço limita o ONNX Runtime a uma thread de CPU para o Raspberry Pi Zero 2 W.
