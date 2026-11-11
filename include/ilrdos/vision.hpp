// ILrDOS — visão: detecção (YuNet), reconhecimento (SFace) e cadastro consentido de rostos.
// Nenhuma imagem é gravada ou enviada pela rede; só vetores numéricos ficam no FaceStore.
#pragma once

#ifdef ILRDOS_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>
#include <opencv2/videoio.hpp>
#endif

#include <functional>

#include "faces.hpp"

namespace ilrdos {

struct Person {
    double cx{0.5}, cy{0.5}, w{0.0}, h{0.0};   // centro e tamanho normalizados (0..1) no quadro
    double detScore{1.0};
    std::string name;                           // vazio = desconhecido
    double matchScore{0.0};
    std::vector<float> feature;                 // vazio quando o reconhecimento não rodou
};

struct Observation {
    int faces{-1};                              // -1 = a varredura falhou (câmera/modelo)
    std::vector<Person> people;
    std::vector<std::string> knownNames() const {
        std::vector<std::string> out;
        for (const auto& p : people) if (!p.name.empty() && std::find(out.begin(), out.end(), p.name) == out.end()) out.push_back(p.name);
        return out;
    }
    int unknownCount() const {
        int count = 0;
        for (const auto& p : people) if (p.name.empty()) ++count;
        return std::max(count, faces > 0 ? faces - static_cast<int>(people.size()) + count : 0);
    }
};

inline std::string describePeople(const Observation& obs) {
    if (obs.faces < 0) return "Não consegui enxergar agora (câmera indisponível).";
    if (obs.faces == 0) return "Não vejo nenhum rosto agora (isso não prova que a sala está vazia).";
    const auto known = obs.knownNames();
    std::string out = "Vejo " + std::to_string(obs.faces) + (obs.faces == 1 ? " rosto" : " rostos");
    if (!known.empty()) {
        out += ". Reconheço: ";
        for (std::size_t i = 0; i < known.size(); ++i) out += (i ? ", " : "") + FaceStore::displayName(known[i]);
    }
    const int unknown = obs.unknownCount();
    if (unknown > 0) out += std::string(known.empty() ? ". " : "; ") + std::to_string(unknown) + (unknown == 1 ? " desconhecido" : " desconhecidos");
    return out + ".";
}

class VisionSystem {
public:
    using EnrollCallback = std::function<void(bool ok, const std::string& name, const std::string& message)>;
    static constexpr int kEnrollSamples = 5;

    VisionSystem(std::string detectorModel, std::string recognizerModel, int cameraIndex, bool simulate, FaceStore& store)
        : detectorModel_(std::move(detectorModel)), recognizerModel_(std::move(recognizerModel)),
          cameraIndex_(cameraIndex), simulate_(simulate), store_(store) {}

    void setEnrollCallback(EnrollCallback callback) { enrollCallback_ = std::move(callback); }

    int lastFaces() const { std::lock_guard<std::mutex> lock(mutex_); return last_.faces; }
    Observation last() const { std::lock_guard<std::mutex> lock(mutex_); return last_; }
    std::string status() const { std::lock_guard<std::mutex> lock(mutex_); return lastResult_; }
    bool recognitionAvailable() const { std::lock_guard<std::mutex> lock(mutex_); return recognitionOk_; }

    bool enrolling() const { std::lock_guard<std::mutex> lock(mutex_); return enroll_.active; }

    // Inicia o cadastro (consentimento já obtido). O progresso acontece nas próximas varreduras.
    std::string startEnroll(const std::string& name) {
        if (!FaceStore::validName(name)) return "Nome inválido (use letras, números, _ ou -; até 32 caracteres).";
        std::lock_guard<std::mutex> lock(mutex_);
        if (enroll_.active) return "Já estou cadastrando " + FaceStore::displayName(enroll_.name) + "; aguarde.";
        enroll_ = Enroll{};
        enroll_.active = true;
        enroll_.name = name;
        enroll_.started = Clock::now();
        keepOpenUntil_ = Clock::now() + std::chrono::seconds(35);
        return "Cadastro de " + FaceStore::displayName(name) + " iniciado: fique sozinho(a) diante da câmera por alguns segundos.";
    }

    void cancelEnroll() { std::lock_guard<std::mutex> lock(mutex_); enroll_ = Enroll{}; }

    // Observação de no máximo `maxAgeMs` atrás; faz uma varredura nova se estiver velha.
    Observation recent(int maxAgeMs) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lastScanAt_ != Clock::time_point{} && Clock::now() - lastScanAt_ < std::chrono::milliseconds(maxAgeMs)) return last_;
        }
        return scan();
    }

    Observation scan() {
        Observation obs;
        std::optional<std::tuple<bool, std::string, std::string>> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            obs = simulate_ ? scanSimulated() : scanCamera();
            last_ = obs;
            lastScanAt_ = Clock::now();
            lastResult_ = obs.faces < 0 ? (lastResult_.empty() ? "Visão indisponível.\n" : lastResult_)
                                        : "Visão: " + describePeople(obs) + "\n";
            if (!obs.people.empty() || obs.faces < 0 || enroll_.active) finished = stepEnrollLocked(obs);
        }
        if (finished && enrollCallback_) enrollCallback_(std::get<0>(*finished), std::get<1>(*finished), std::get<2>(*finished));
        return obs;
    }

private:
    struct Enroll {
        bool active{false};
        std::string name;
        std::vector<std::vector<float>> samples;
        Clock::time_point started{}, lastSample{};
    };

    std::optional<std::tuple<bool, std::string, std::string>> stepEnrollLocked(const Observation& obs) {
        if (!enroll_.active) return std::nullopt;
        auto finish = [&](bool ok, const std::string& message) {
            const std::string name = enroll_.name;
            enroll_ = Enroll{};
            logLine("rostos", message);
            return std::make_optional(std::make_tuple(ok, name, message));
        };
        const auto now = Clock::now();
        if (now - enroll_.started > std::chrono::seconds(25)) {
            return finish(false, "Cadastro cancelado: não consegui ver um rosto sozinho e nítido a tempo.");
        }
        if (!recognitionOk_) return finish(false, "Cadastro impossível: modelo de reconhecimento (SFace) ausente.");
        if (obs.people.size() != 1 || obs.faces != 1) return std::nullopt;
        const Person& person = obs.people.front();
        if (person.feature.empty() || person.detScore < 0.80 || person.w < 0.12) return std::nullopt;
        if (now - enroll_.lastSample < std::chrono::milliseconds(350)) return std::nullopt;
        if (!person.name.empty() && person.name != enroll_.name) {
            return finish(false, "Esse rosto já está cadastrado como " + FaceStore::displayName(person.name) + ".");
        }
        enroll_.samples.push_back(person.feature);
        enroll_.lastSample = now;
        if (static_cast<int>(enroll_.samples.size()) < kEnrollSamples) return std::nullopt;
        const std::string error = store_.add(enroll_.name, enroll_.samples);
        if (!error.empty()) return finish(false, error);
        return finish(true, "Rosto de " + FaceStore::displayName(enroll_.name) + " cadastrado.");
    }

    // ---- simulação: ILRDOS_SIM_FACES_FILE com "N" (N rostos desconhecidos) ou linhas "identidade [cx] [largura]".
    Observation scanSimulated() {
        Observation obs;
        recognitionOk_ = true;
        std::ifstream input(envOr("ILRDOS_SIM_FACES_FILE", "/nonexistent"));
        std::vector<std::tuple<std::string, double, double>> entries;
        if (input) {
            std::string line;
            while (std::getline(input, line)) {
                const auto words = splitWords(line);
                if (words.empty()) continue;
                bool numeric = !words[0].empty() && std::all_of(words[0].begin(), words[0].end(), [](char c) { return c >= '0' || c == '-'; }) &&
                               (std::isdigit(static_cast<unsigned char>(words[0][0])) || words[0][0] == '-');
                if (numeric && words.size() == 1) {
                    int n = 0;
                    try { n = std::stoi(words[0]); } catch (...) {}
                    if (n < 0) { obs.faces = -1; return obs; }   // -1 simula falha da câmera
                    for (int i = 0; i < std::min(n, 20); ++i) entries.emplace_back("?" + std::to_string(i), 0.5, 0.3);
                    continue;
                }
                double cx = 0.5, w = 0.3;
                try { if (words.size() > 1) cx = std::stod(words[1]); if (words.size() > 2) w = std::stod(words[2]); } catch (...) {}
                entries.emplace_back(words[0], cx, w);
            }
        }
        obs.faces = static_cast<int>(entries.size());
        for (const auto& [label, cx, w] : entries) {
            Person person;
            person.cx = cx; person.w = w; person.h = w; person.detScore = 0.95;
            if (store_.names().empty() && !enroll_.active) {
                // sem cadastros e sem cadastro em curso: não precisa calcular o vetor
            } else {
                person.feature = simulatedFeature(label, ++simCounter_);
                const FaceMatch match = store_.match(person.feature);
                person.name = match.name;
                person.matchScore = match.score;
            }
            obs.people.push_back(std::move(person));
        }
        return obs;
    }

    Observation scanCamera() {
        Observation obs;
#ifdef ILRDOS_HAVE_OPENCV
        try {
            const auto now = Clock::now();
            const bool fresh = !camera_.isOpened();
            if (fresh) {
                camera_.open(cameraIndex_, cv::CAP_V4L2);
                if (!camera_.isOpened()) {
                    lastResult_ = "Visão indisponível: não consegui abrir a câmera " + std::to_string(cameraIndex_) + ".\n";
                    return obs;
                }
                camera_.set(cv::CAP_PROP_FRAME_WIDTH, 640);
                camera_.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
                camera_.set(cv::CAP_PROP_BUFFERSIZE, 1);
            }
            const int discard = fresh ? 3 : (now - lastRead_ > std::chrono::seconds(1) ? 2 : 0);
            for (int i = 0; i < discard; ++i) camera_.grab();
            cv::Mat frame;
            if (!camera_.read(frame) || frame.empty()) {
                camera_.release();
                lastResult_ = "Visão indisponível: a câmera não forneceu uma imagem.\n";
                return obs;
            }
            lastRead_ = Clock::now();

            if (!detector_) {
                detector_ = cv::FaceDetectorYN::create(detectorModel_, "", cv::Size(320, 240), 0.75F, 0.3F, 5000,
                                                       cv::dnn::DNN_BACKEND_OPENCV, cv::dnn::DNN_TARGET_CPU);
            }
            if (!recognizer_ && !recognizerTried_) {
                recognizerTried_ = true;
                if (std::ifstream(recognizerModel_).good()) {
                    try { recognizer_ = cv::FaceRecognizerSF::create(recognizerModel_, ""); } catch (const cv::Exception&) {}
                }
                if (!recognizer_) logLine("visao", "modelo SFace ausente: só detecto rostos, não identifico.");
            }
            recognitionOk_ = static_cast<bool>(recognizer_);

            const double scale = std::min(1.0, 320.0 / std::max(frame.cols, frame.rows));
            cv::Mat resized;
            cv::resize(frame, resized, cv::Size(std::max(1, static_cast<int>(frame.cols * scale)),
                                                std::max(1, static_cast<int>(frame.rows * scale))), 0, 0, cv::INTER_AREA);
            detector_->setInputSize(resized.size());
            cv::Mat detections;
            detector_->detect(resized, detections);

            obs.faces = detections.empty() ? 0 : detections.rows;
            const bool needFeatures = recognitionOk_ && (enroll_.active || !store_.names().empty());
            // reconhece só os 3 maiores rostos (o custo cresce com cada rosto)
            std::vector<int> order;
            for (int i = 0; i < obs.faces; ++i) order.push_back(i);
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return detections.at<float>(a, 2) * detections.at<float>(a, 3) > detections.at<float>(b, 2) * detections.at<float>(b, 3);
            });
            for (std::size_t k = 0; k < order.size() && k < 3; ++k) {
                const int i = order[k];
                Person person;
                person.cx = (detections.at<float>(i, 0) + detections.at<float>(i, 2) / 2.0F) / resized.cols;
                person.cy = (detections.at<float>(i, 1) + detections.at<float>(i, 3) / 2.0F) / resized.rows;
                person.w = detections.at<float>(i, 2) / resized.cols;
                person.h = detections.at<float>(i, 3) / resized.rows;
                person.detScore = detections.at<float>(i, 14);
                if (needFeatures) {
                    cv::Mat row = detections.row(i).clone();
                    for (int c = 0; c < 14; ++c) row.at<float>(0, c) = static_cast<float>(row.at<float>(0, c) / scale);
                    cv::Mat aligned, feature;
                    recognizer_->alignCrop(frame, row, aligned);
                    recognizer_->feature(aligned, feature);
                    feature = feature.clone();
                    person.feature.assign(feature.ptr<float>(), feature.ptr<float>() + feature.total());
                    const FaceMatch match = store_.match(person.feature);
                    person.name = match.name;
                    person.matchScore = match.score;
                }
                obs.people.push_back(std::move(person));
            }
            if (obs.faces > 0 || enroll_.active) keepOpenUntil_ = Clock::now() + std::chrono::seconds(10);
            if (Clock::now() >= keepOpenUntil_) camera_.release();   // sem ninguém: não deixa a câmera aberta
        } catch (const cv::Exception& error) {
            std::string detail = std::string(error.what()).substr(0, 180);
            std::replace(detail.begin(), detail.end(), '\n', ' ');
            lastResult_ = "Visão indisponível: falha da câmera/modelo (" + detail + ").\n";
            obs = Observation{};
        }
#else
        lastResult_ = "Visão não compilada. Instale OpenCV 4 de desenvolvimento e recompile o ILrDOS.\n";
#endif
        return obs;
    }

    std::string detectorModel_, recognizerModel_;
    int cameraIndex_{0};
    bool simulate_{false};
    FaceStore& store_;
    EnrollCallback enrollCallback_;
    Observation last_;
    Clock::time_point lastScanAt_{};
    Enroll enroll_;
    bool recognitionOk_{false};
    unsigned simCounter_{0};
    Clock::time_point keepOpenUntil_{};
#ifdef ILRDOS_HAVE_OPENCV
    cv::VideoCapture camera_;
    cv::Ptr<cv::FaceDetectorYN> detector_;
    cv::Ptr<cv::FaceRecognizerSF> recognizer_;
    bool recognizerTried_{false};
    Clock::time_point lastRead_{};
#endif
    mutable std::mutex mutex_;
    std::string lastResult_{"Visão aguardando uma varredura. Use: vision scan.\n"};
};

}  // namespace ilrdos
