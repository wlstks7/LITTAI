﻿#if defined _WIN32 || defined _WIN64
#pragma warning( disable : 4290 )
#define _USE_MATH_DEFINES
#endif

#include <numeric>
#include <aruco.h>
#include <opencv2/opencv.hpp>
#include <polypartition.h>
#include "marker_tracker.h"

using namespace Littai;


namespace
{
    template <class T>
    inline auto len(const T& v) -> decltype(v.x)
    {
        return std::sqrt(v.x * v.x + v.y * v.y);
    }

    template <class T, class U>
    inline auto dot(const T& v1, const U& v2) -> decltype(v1.x * v2.x)
    {
        return v1.x * v2.x + v1.y * v2.y;
    }

    template <class T>
    inline T normalize(const T& v)
    {
        return v * (1.0 / len(v));
    }

    template <class T>
    inline T toUnit(const T& v, const double scaleX, const double scaleY)
    {
        return T(2.0 * v.x / scaleX - 1.0, 1.0 - 2.0 * v.y / scaleY);
    }

    template <class T>
    inline int sign(const T& v)
    {
        return v >= 0 ? 1 : -1;
    }

    template <class T>
    inline T rotate(const T& pos, double angle)
    {
        return T(pos.x * cos(angle) - pos.y * sin(angle), pos.x * sin(angle) + pos.y * cos(angle));
    }

    template <class T>
    inline T toLocal(const T& pos, const T& parentPos, double parentAngle)
    {
        return rotate(T(pos.x - parentPos.x, pos.y - parentPos.y), -parentAngle);
    }
}


int TrackedEdge::currentId = 0;


MarkerTracker::MarkerTracker(QQuickItem *parent)
    : Image(parent)
    , isFinished_(false)
    , contrastThreshold_(100)
    , contrastThresholdMin_(50)
    , contrastThresholdMax_(100)
    , contrastThresholdStep_(10)
    , fps_(30)
    , predictionFrame_(0)
    , frameCount_(0)
    , startTime_(std::chrono::system_clock::now())
{
    thread_ = std::thread([&] {
        using namespace std::chrono;
        while (!isFinished_) {
            const auto t1 = high_resolution_clock::now();
            track();
            ++frameCount_;
            const auto t2 = high_resolution_clock::now();
            const auto dt = t2 - t1;
            const auto waitTime = microseconds(1000000 / fps_) - dt;
            if (waitTime > microseconds::zero()) {
                std::this_thread::sleep_for(waitTime);
            }
        }
    });
}


MarkerTracker::~MarkerTracker()
{
    isFinished_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}


void MarkerTracker::setInputImage(const QVariant &image)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        image.value<cv::Mat>().copyTo(inputImage_);
        isImageUpdated_ = true;
    }
    emit inputImageChanged();
}


QVariant MarkerTracker::inputImage() const
{
    cv::Mat image;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inputImage_.copyTo(image);
    }
    return QVariant::fromValue(image);
}


void MarkerTracker::track()
{
    if (!isImageUpdated_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inputImage_.empty()) return;
    }

    cv::Mat image, gray;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inputImage_.copyTo(image);
    }

    cv::Mat rawGray;
    cv::cvtColor(image, rawGray, cv::COLOR_BGR2GRAY);

    // cv::Mat raw;
    // cv::cvtColor(image, raw, cv::COLOR_BGR2GRAY);

    // ガンマ補正 / 複数枚平均
    preProcess(image);

    // グレースケール
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);

    // マーカ認識
    detectMarkers(image, rawGray);

    // ポリゴン認識
    detectPolygons(image, gray);

    // 認識できなかった時に速度と回転を推定
    // detectMotions(image, gray);

    // エッジの配置からルールベースでパターンを認識
    detectPatterns(image, gray);

    setImage(image, false);

    isImageUpdated_ = false;
}


void MarkerTracker::preProcess(cv::Mat &image)
{
    // ノイズリダクションと 2 値化
    // OTSU も試してみたけど、アダプティブにしてしまうと安定しない..
    cv::threshold(image, image, contrastThreshold_, 255, cv::THRESH_BINARY);
    cv::medianBlur(image, image, 3);
    // cv::dilate(image, image, cv::Mat(), cv::Point(-1, -1), 1);

    /*
    imageCaches_.push_back(image.clone());
    if (imageCaches_.size() > 3) {
        imageCaches_.pop_front();
    }
    */
    /*
    cv::Mat input(image.rows, image.cols, image.type(), cv::Scalar(0));
    for (auto&& cache : imageCaches_) {
        input += cache * 1.0 / imageCaches_.size();
    }
    image = input;
    */
}


void MarkerTracker::detectMarkers(cv::Mat &resultImage, cv::Mat &inputImage)
{
    const double scale = 1.0;
    cv::Mat image;
    cv::resize(inputImage, image, cv::Size(), scale, scale, cv::INTER_LINEAR);

    // ArUco によるマーカの認識
    // スレッショルドを振って 3 回行う
    aruco::MarkerDetector detector;
    detector.setMinMaxSize(0.01f, 0.07f);
    detector.setThresholdMethod(aruco::MarkerDetector::FIXED_THRES);
    detector.setCornerRefinementMethod(aruco::MarkerDetector::CornerRefinementMethod::SUBPIX);
    detector.setThresholdParams(contrastThreshold_, 0);
    std::vector<int> thresholds;
    for (int t = contrastThresholdMin_; t <= contrastThresholdMax_; t += contrastThresholdStep_) {
        thresholds.push_back(t);
    }
    std::vector<std::vector<aruco::Marker>> tmpMarkersList(thresholds.size());

    #pragma omp parallel for
    for (unsigned int i = 0; i < thresholds.size(); ++i) {
        cv::Mat binaryImage, filteredImage;
        cv::threshold(image, binaryImage, thresholds[i], 255, cv::THRESH_BINARY);
        cv::medianBlur(binaryImage, filteredImage, 3);
        detector.detect(filteredImage, tmpMarkersList[i]);
    }

    std::vector<aruco::Marker> markers;
    for (const auto& tmpMarkers : tmpMarkersList) {
        for (const auto& marker : tmpMarkers) {
            const auto it = std::find_if(markers.begin(), markers.end(), [&marker](const aruco::Marker& existingMarker) {
                return marker.id == existingMarker.id;
            });
            if (it == markers.end()) {
                markers.push_back(marker);
            }
        }
    }

    // 結果を格納
    std::vector<TrackedMarker> newMarkers;
    for (auto&& marker : markers) {
        const auto sum = std::accumulate(marker.begin(), marker.end(), cv::Point2f(0.f));

        TrackedMarker info;
        info.id = marker.id;
        info.x = sum.x / marker.size() / scale;
        info.y = sum.y / marker.size() / scale;
        const auto side = marker[0] - marker[3];
        info.angle = std::atan2(side.x, side.y);
        info.size = len(side);

        newMarkers.push_back(info);
    }

    // フラグをオフにしておく
    for (auto&& marker : markers_) {
        marker.checked = false;
    }

    // 見つかったアイテムは情報を更新してフラグを立てる
    for (auto&& newMarker : newMarkers) {
        bool isFound = false;
        for (auto&& marker : markers_) {
            if (newMarker.id == marker.id) {
                isFound = true;
                marker.update(newMarker);
                marker.checked = true;
                break;
            }
        }
        if (!isFound) {
            markers_.push_back(newMarker);
        }
    }

    // しばらく認識されなかったマーカは削除
    // 認識されたアイテムはフレームカウントを増加
    for (auto it = markers_.begin(); it != markers_.end();) {
        auto& marker = *it;
        if (marker.checked) {
            marker.lostCount = 0;
        } else {
            marker.lostCount++;
            if (marker.lostCount > 30) {
                it = markers_.erase(it);
                continue;
            }
        }
        ++marker.frameCount;
        ++it;
    }

    // マーカの位置補正
    predictPosition();

    emit markersChanged();
    /*
    for (auto&& marker : markers_) {
        marker.print();
    }
    */
}


void MarkerTracker::predictPosition()
{
    for (auto&& marker : markers_) {
        if (!marker.checked) continue;

        const auto dx = marker.x - marker.px;
        const auto dy = marker.y - marker.py;
        marker.px = marker.x;
        marker.py = marker.y;

        const auto now = std::chrono::system_clock::now();
        const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(now - marker.t).count();
        marker.t = now;

        if (marker.frameCount < 2) continue;

        marker.vx += (dx / dt - marker.vx) * 0.5;
        marker.vy += (dy / dt - marker.vy) * 0.5;

        const double predictionDuration = (1000.0 * 1000.0) / fps_ * predictionFrame_;
        marker.x += marker.vx * predictionDuration;
        marker.y += marker.vy * predictionDuration;
    }
}


void MarkerTracker::detectPolygons(cv::Mat &resultImage, cv::Mat &inputImage)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 領域を抽出
    std::vector<std::vector<cv::Point>> contours;
    auto image = inputImage.clone();
    cv::dilate(image, image, cv::Mat());
    cv::findContours(image, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_TC89_KCOS);

    // マーカを囲む領域を認識
    std::map<unsigned int, std::vector<cv::Point>> contourMap;
    if (contours.size() > 0 && markers_.size() > 0) {
        // 領域を大きい順に並べる
        std::sort(contours.begin(), contours.end(),
            [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
                return cv::contourArea(a) > cv::contourArea(b);
            });

        // マーカを内包する領域を調べる
        for (auto&& marker : markers_) {
            bool isFound = false;
            for (const auto& contour : contours) {
                // マーカの中心座標が領域内に含まれるか調べる
                // 含まれていればマップに登録
                const cv::Point pt(marker.x, marker.y);
                if (cv::pointPolygonTest(contour, pt, 0) == 1) {
                    contourMap.emplace(marker.id, contour);
                    isFound = true;
                    break;
                }
            }
            if (!isFound) {
                continue;;
                // qDebug("marker detected but contour not detected.");
            }
        }
    }

    for (auto&& marker : markers_) {
        const cv::Point center(marker.x, marker.y);

        auto it = contourMap.find(marker.id);
        if (it == contourMap.end()) continue;
        auto& contour = (*it).second;

        std::vector<std::vector<cv::Point>> contours = { contour };
        cv::drawContours(resultImage, contours, 0, cv::Scalar(255, 0, 0), 3);

        // ポリゴン認識
        std::vector<cv::Point> polygon;
        cv::approxPolyDP(contour, polygon, 5, true);
        std::reverse(polygon.begin(), polygon.end());

        // 多すぎる場合はスキップ
        if (polygon.size() > 50) continue;

        // 認識点群が角張っていることを利用して棄却
        {
            // 近い３点のなす角度が大きすぎる/小さすぎる場合、中心点を棄却
            std::vector<cv::Point> filteredPolygon;
            for (unsigned int i = 0; i < polygon.size(); ++i) {
                const auto i1 = i;
                const auto i2 = (i + 1) % polygon.size();
                const auto i3 = (i + 2) % polygon.size();
                const auto v1 = polygon[i2] - polygon[i1];
                const auto v2 = polygon[i2] - polygon[i3];
                const auto minLenThresh = inputImage.cols * 0.03;
                const auto angle = acos(abs(dot(normalize(cv::Point2d(v1)), normalize(cv::Point2d(v2)))));
                const auto angleThresh = 0.2 * M_PI;
                if ((angle < angleThresh) || (angle > M_PI - angleThresh)) {
                    ++i; // i2 をスキップ
                } else if (len(v1) < minLenThresh && len(v2) < minLenThresh) {
                    ++i;
                }
                filteredPolygon.push_back(polygon[i1]);
            }
            polygon = filteredPolygon;
        }
        marker.polygon = polygon;

        // 三角ポリゴン化
        marker.indices = triangulatePolygons(polygon);

        // 突端認識
        if (polygon.size() >= 4) {
            std::vector<TrackedEdge> edges;
            const int N = polygon.size();
            for (int i = 0; i < N; ++i) {
                // 隣り合う 4 点
                const int i0 = i;
                const int i1 = ((i + 1) < N) ? (i + 1) : (i + 1 - N);
                const int i2 = ((i + 2) < N) ? (i + 2) : (i + 2 - N);
                const int i3 = ((i + 3) < N) ? (i + 3) : (i + 3 - N);
                const auto v0 = polygon[i0];
                const auto v1 = polygon[i1];
                const auto v2 = polygon[i2];
                const auto v3 = polygon[i3];

                // 4 点を作る辺
                const auto s1 = v1 - v0;
                const auto s2 = v2 - v1;
                const auto s3 = v3 - v2;
                const auto l1 = len(s1);
                const auto l2 = len(s2);
                const auto l3 = len(s3);

                // 4 点で囲まれる中心座標
                const auto averagePos = (v0 + v1 + v2 + v3) * 0.25;
                const double ratio = 0.4;

                // 4 点の方向
                // const auto dir = ((v1 + v2) - (v0 + v3)) * 0.5;
                // 短い方を基点とする方向を求める
                const auto dir = (l1 < l3) ? s1 : -s3;

                // 認識範囲
                const auto margin = 10;
                const cv::Rect area(
                    cv::Point(margin, margin),
                    cv::Point(inputImage.cols - margin, inputImage.rows - margin));

                // 中心の辺が短く、1 番目と 3 番目の辺が逆を向き、中心が白くて、
                // 遠くにある場合、突端として認識する
                const bool isMiddleShort     = (l1 != 0 && l2 / l1 < ratio) && (l3 != 0 && l2 / l3 < ratio);
                const bool isOpposite        = s1.dot(s3) < -0.75;
                const bool isAveragePosInner = cv::pointPolygonTest(polygon, averagePos, false) >= 0.0;
                const bool isFar             = len((v1 + v2) * 0.5 - center) > 40;
                const bool isMiddleMinLen    = len(s2) > 8;
                const bool isNotNearBoundary = area.contains(v0) && area.contains(v1) && area.contains(v2) && area.contains(v3);
                // qDebug() << isMiddleShort << " " << isOpposite << " " << isAveragePosInner << " " << isFar << " " << isMiddleMinLen;
                if (isMiddleShort && isOpposite && isAveragePosInner && isFar && isMiddleMinLen && isNotNearBoundary) {
                    TrackedEdge edge((v1 + v2) * 0.5);
                    edge.direction = dir;
                    edges.push_back(edge);
                }
            }

            // Raw の Edge を描画
            for (const auto& edge : edges) {
                cv::circle(resultImage, edge, 6, cv::Scalar(0, 255, 0), 2);
            }

            // 過去に登録されたエッジと比較して近いものは更新
            // 新しいものは追加
            for (auto&& existingEdge : marker.edges) {
                existingEdge.checked = false;
            }
            std::vector<TrackedEdge> newEdges;
            for (auto&& newEdge : edges) {
                bool isFound = false;
                for (auto&& existingEdge : marker.edges) {
                    // NOTE: 近くなくても登録するようにしてみた
                    if (!existingEdge.checked && len(newEdge - existingEdge) < inputImage.cols * 0.5) {
                        existingEdge.x = newEdge.x;
                        existingEdge.y = newEdge.y;
                        existingEdge.direction = newEdge.direction;
                        existingEdge.checked = true;
                        isFound = true;
                        break;
                    }
                }
                if (!isFound) {
                    newEdge.id = TrackedEdge::GetId();
                    newEdges.push_back(newEdge);
                }
            }

            // 一定期間見つからなかったら削除
            for (auto it = marker.edges.begin(); it != marker.edges.end();) {
                auto& edge = *it;
                if (edge.checked) {
                    edge.lostCount = 0;
                    ++edge.frameCount;
                    if (edge.frameCount > 5) {
                        edge.activated = true;
                    }
                } else {
                    ++edge.lostCount;
                    if (edge.lostCount > 3) {
                        edge.activated = false;
                    }
                    if (edge.lostCount > 8) {
                        it = marker.edges.erase(it);
                        continue;
                    }
                }
                ++it;
            }

            // 新しいエッジを追加
            for (const auto& newEdge : newEdges) {
                marker.edges.push_back(newEdge);
            }
        }

        if (polygon.size() >= 3) {
            marker.bound = cv::boundingRect(polygon);
        }
    }

    for (auto&& marker : markers_) {
        // 中心と領域を描画
        const cv::Point center(marker.x, marker.y);
        cv::circle(resultImage, center, marker.size / 2, cv::Scalar(0, 255, 0), 2);

        for (const auto& vertex : marker.polygon) {
            cv::circle(resultImage, vertex, 3, cv::Scalar(0, 0, 255), 2);
        }

        // 認識したエッジを描画
        for (const auto& edge : marker.edges) {
            if (edge.activated) {
                cv::circle(resultImage, edge, 6, cv::Scalar(0, 255, 255), -1);
                const cv::Point2d from = edge;
                const cv::Point2d to = from + edge.direction * 6;
                cv::arrowedLine(resultImage, from, to, CV_RGB(0, 255, 0), 1);
            } else {
                cv::circle(resultImage, edge, 3, cv::Scalar(0, 255, 255), 1);
            }
        }

        // イメージ格納
        marker.image = resultImage(marker.bound).clone();
    }
}


void MarkerTracker::detectMotions(cv::Mat &resultImage, cv::Mat &inputImage)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (historyImage_.empty()) {
        historyImage_ = cv::Mat(inputImage.rows, inputImage.cols, CV_32FC1);
    }

    cv::Mat currentImage;
    cv::medianBlur(inputImage, currentImage, 3);
    cv::dilate(currentImage, currentImage, cv::Mat(), cv::Point(-1, -1), 2, 1, 1);
    if (preImage_.empty()) {
        preImage_ = currentImage.clone();
        return;
    }

    cv::Mat diff;
    cv::absdiff(currentImage, preImage_, diff);
    preImage_ = currentImage.clone();

    using namespace std::chrono;
    const auto dt = system_clock::now() - startTime_;
    const auto ms = duration_cast<milliseconds>(dt);
    const auto duration = 1000.0 / fps_ * 3;
    cv::updateMotionHistory(diff, historyImage_, ms.count(), duration);
    cv::Mat mgMask, mgOrientation;
    cv::calcMotionGradient(historyImage_, mgMask, mgOrientation, 1, 1000000, 3);
    cv::Mat segMask;
    std::vector<cv::Rect> segBounds;
    cv::segmentMotion(historyImage_, segMask, segBounds, ms.count(), duration);

    const int minArea = 3000; // pixel x pixel
    for (const auto& rect : segBounds) {
        if (rect.area() > minArea) {
            cv::rectangle(resultImage, rect, cv::Scalar(0, 255, 255), 2);

            // マーカーの中で今回のフレームで見つかっていないモノをトラッキング情報を使って更新する
            for (auto&& marker : markers_) {
                const auto mx = marker.bound.x + marker.bound.width  / 2;
                const auto my = marker.bound.y + marker.bound.height / 2;
                if (rect.contains(cv::Point(marker.x, marker.y)) ||
                    rect.contains(cv::Point(mx, my))) {
                    cv::rectangle(resultImage, rect, cv::Scalar(0, 0, 255), 3);
                    const auto orientRoi = mgOrientation(rect);
                    const auto maskRoi = mgMask(rect);
                    const auto historyRoi = historyImage_(rect);
                    const auto angle = -cv::calcGlobalOrientation(orientRoi, maskRoi, historyRoi, ms.count(), duration);
                    const auto cx = rect.x + rect.width  / 2;
                    const auto cy = rect.y + rect.height / 2;

                    auto dx = cx - marker.trackedX;
                    auto dy = cy - marker.trackedY;
                    auto da = angle - marker.trackedAngle;
                    if (da >  180) da -= 360;
                    if (da < -180) da += 360;

                    // 回転のエラーの棄却と
                    // 大きい時は位置ずれが大きいので補正する
                    const double maxAngle = 30.0;
                    if (abs(da) > maxAngle) da = 0;
                    dx *= (1.0 - pow(abs(da) / maxAngle, 0.5));
                    dy *= (1.0 - pow(abs(da) / maxAngle, 0.5));
                    const double maxDiff = 10.0;
                    dx = max(-maxDiff, min(maxDiff, dx));
                    dy = max(-maxDiff, min(maxDiff, dy));
                    da *= M_PI / 180;

                    cv::arrowedLine(resultImage, cv::Point(marker.trackedX, marker.trackedY), cv::Point(cx, cy), cv::Scalar(0, 0, 255), 4);

                    marker.trackedX = cx;
                    marker.trackedY = cy;
                    marker.trackedAngle = angle;
                    if (marker.checked) {
                        continue;
                    }

                    marker.x += dx;
                    marker.y += dy;
                    marker.angle += da;
                    for (auto&& vert : marker.polygon) {
                        const auto lx = vert.x - (marker.x - dx);
                        const auto ly = vert.y - (marker.y - dy);
                        vert.x = marker.x + (lx * cos(-da) - ly * sin(-da));
                        vert.y = marker.y + (lx * sin(-da) + ly * cos(-da));
                    }
                    for (auto&& edge : marker.edges) {
                        const auto preEdge = edge;
                        const auto lx = edge.x - (marker.x - dx);
                        const auto ly = edge.y - (marker.y - dy);
                        edge.x = marker.x + (lx * cos(-da) - ly * sin(-da));
                        edge.y = marker.y + (lx * sin(-da) + ly * cos(-da));
                        const auto dirX = edge.direction.x;
                        const auto dirY = edge.direction.y;
                        edge.direction.x = dirX * cos(-da) - dirY * sin(-da);
                        edge.direction.y = dirX * sin(-da) + dirY * cos(-da);
                        cv::line(resultImage, preEdge, edge, cv::Scalar(255, 0, 0), 2);
                        // edge.lostCount -= 1;
                        // if (edge.lostCount < 0) edge.lostCount = 0;
                    }

                    marker.lostCount = 0;
                }
            }
        }
    }
}


void MarkerTracker::detectPatterns(cv::Mat &resultImage, cv::Mat &inputImage)
{
    const auto width  = inputImage_.cols;
    const auto height = inputImage_.rows;

    for (auto&& marker : markers_) {
        const auto& edges = marker.edges;
        const auto pos     = cv::Point2d(marker.x, marker.y);
        const auto forward = cv::Point2d(cos(-marker.angle), sin(-marker.angle));
        const auto right   = cv::Point2d(cos(-marker.angle + M_PI / 2), sin(-marker.angle + M_PI / 2));
        cv::arrowedLine(resultImage, pos, pos + forward * 30, cv::Scalar(255, 0, 255), 2);
        cv::arrowedLine(resultImage, pos, pos + right   * 30, cv::Scalar(255, 255, 0), 2);

        const int N = static_cast<int>(edges.size());
        if (N < 2) continue;

        std::vector<TrackedPattern> patterns;

        // 2 個からなるルール
        for (int i = 0; i < N; ++i) {
            for (int j = i + 1; j < N; ++j) {
                const auto& edgeA = edges[i];
                const auto& edgeB = edges[j];
                if (!edgeA.activated || !edgeB.activated) continue;

                const auto markerPos = toUnit(cv::Point2d(marker.x, marker.y), width, height);
                const auto posA = toUnit<cv::Point2d>(edgeA, width, height);
                const auto posB = toUnit<cv::Point2d>(edgeB, width, height);
                const auto vecA = cv::Point2d(edgeA.direction.x / width, edgeA.direction.y / height) * 2;
                const auto vecB = cv::Point2d(edgeB.direction.x / width, edgeB.direction.y / height) * 2;
                const auto lenA = len(vecA);
                const auto lenB = len(vecB);
                const auto baseA = toUnit(cv::Point2d(edgeA) - edgeA.direction - cv::Point2d(marker.x, marker.y), width, height);
                const auto baseB = toUnit(cv::Point2d(edgeB) - edgeB.direction - cv::Point2d(marker.x, marker.y), width, height);
                const auto dirA = normalize(edgeA.direction);
                const auto dirB = normalize(edgeB.direction);
                const auto lenAB = len(posA - posB);
                const auto dirAB = normalize(posB - posA);

                cv::circle(resultImage, edgeA, 10, cv::Scalar(0, 0, 255), 2);
                cv::circle(resultImage, edgeB, 10, cv::Scalar(0, 0, 255), 2);
                cv::circle(resultImage, cv::Point2d(edgeA) - edgeA.direction, 5, cv::Scalar(0, 0, 255), 2);
                cv::circle(resultImage, cv::Point2d(edgeB) - edgeB.direction, 5, cv::Scalar(0, 0, 255), 2);

                const double parallelThresh = cos(M_PI / 6);
                const bool isParallel =
                    (abs(dot(forward, dirA)) > parallelThresh && abs(dot(forward, dirB)) > parallelThresh && (sign(dot(forward, dirA)) == sign(dot(forward, dirB)))) ||
                    (abs(dot(right,   dirA)) > parallelThresh && abs(dot(right,   dirB)) > parallelThresh && (sign(dot(right,   dirA)) == sign(dot(right,   dirB))));
                const double oppositeThresh = cos(M_PI / 4);
                const bool isOpposite =
                    (abs(dot(forward, dirA)) > oppositeThresh && abs(dot(forward, dirB)) > oppositeThresh && (sign(dot(forward, dirA)) != sign(dot(forward, dirB)))) ||
                    (abs(dot(right,   dirA)) > oppositeThresh && abs(dot(right,   dirB)) > oppositeThresh && (sign(dot(right,   dirA)) != sign(dot(right,   dirB))));
                const double verticalThresh = cos(M_PI / 5);
                const bool isVertical =
                    (abs(dot(forward, dirA)) > verticalThresh && abs(dot(right,   dirB)) > verticalThresh) ||
                    (abs(dot(right,   dirA)) > verticalThresh && abs(dot(forward, dirB)) > verticalThresh);
                const bool isBasePosNear = len(baseA - baseB) < 0.2;
                const bool isBasePosNearMarker = len(baseA) < 0.35 && len(baseB) < 0.35;

                const bool isNear = lenAB >= 0.02 && lenAB < 0.15;
                const bool isMid  = lenAB >= 0.15 && lenAB < 0.5;
                const bool isFar  = lenAB >= 0.50 && lenAB < 1.5;

                // qDebug() << isParallel << " " << isOpposite << " " << isVertical << " " << isNear << " " << isMid << " " << isFar << " " << dot(forward, dirA) << " " << dot(forward,   dirB) << " " << dot(right, dirA) << " " << dot(right,   dirB) << " " << lenAB;
                TrackedPattern pattern;
                pattern.edgeIds.push_back(edgeA.id);
                pattern.edgeIds.push_back(edgeB.id);

                // パターン 1
                // ある程度近い 2 点がマーカに対して垂直
                if (isParallel && isNear) {
                    cv::line(resultImage, edgeA, edgeB, cv::Scalar(255, 0, 255), 1);
                    cv::putText(resultImage, "A", (edgeA + edgeB) * 0.5 + cv::Point(dirA * 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 255), 2, CV_AA);
                    pattern.pattern = 1;
                    patterns.push_back(pattern);
                    continue;
                }

                // パターン 2
                // ある程度遠い 2 点がマーカに対して垂直
                if (isParallel && (isMid || isFar)) {
                    cv::line(resultImage, edgeA, edgeB, cv::Scalar(255, 0, 255), 1);
                    cv::putText(resultImage, "B", (edgeA + edgeB) * 0.5 + cv::Point(dirA * 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 255), 2, CV_AA);
                    pattern.pattern = 2;
                    patterns.push_back(pattern);
                    continue;
                }

                // パターン 3
                // 柄がマーカに近接していない遠い 2 点がマーカに対して水平
                // qDebug() << isBasePosNear << " " << len(baseA - baseB) << " " << isBasePosNearMarker << " " << isOpposite << " " << (isMid || isFar);
                if (isBasePosNear && !isBasePosNearMarker && isOpposite && (isMid || isFar)) {
                    cv::line(resultImage, edgeA, edgeB, cv::Scalar(255, 0, 255), 1);
                    cv::putText(resultImage, "C", (edgeA + edgeB) * 0.5 + cv::Point(dirA * 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 255), 2, CV_AA);
                    pattern.pattern = 3;
                    patterns.push_back(pattern);
                    continue;
                }

                // パターン 4
                // マーカに対して L 字になる感じ
                if (isVertical && (isMid || isFar)) {
                    cv::line(resultImage, edgeA, edgeB, cv::Scalar(255, 0, 255), 1);
                    cv::putText(resultImage, "D", (edgeA + edgeB) * 0.5 + cv::Point((dirA + dirB) * 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 255), 2, CV_AA);
                    pattern.pattern = 4;
                    patterns.push_back(pattern);
                    continue;
                }

                // パターン 5
                // 柄がマーカに近接した遠い 2 点がマーカに対して水平
                if (isOpposite && (isMid || isFar)) {
                    cv::line(resultImage, edgeA, edgeB, cv::Scalar(255, 0, 255), 1);
                    cv::putText(resultImage, "E", (edgeA + edgeB) * 0.5 + cv::Point(dirA * 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 255), 2, CV_AA);
                    pattern.pattern = 5;
                    patterns.push_back(pattern);
                    continue;
                }
            }
        }

        marker.patterns = patterns;
    }
}


std::vector<int> MarkerTracker::triangulatePolygons(const std::vector<cv::Point> &polygon)
{
    TPPLPoly poly;
    poly.Init(polygon.size());
    poly.SetHole(false);

    for (unsigned int i = 0; i < polygon.size(); ++i) {
        poly[i].x = polygon[i].x;
        poly[i].y = polygon[i].y;
    }

    TPPLPartition pp;
    std::list<TPPLPoly> result;
    pp.Triangulate_EC(&poly, &result);

    std::vector<int> indices;
    for (auto&& triangle : result) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < poly.GetNumPoints(); ++j) {
                if (triangle[i] == poly[j]) {
                    indices.push_back(j);
                    break;
                }
            }
        }
    }

    return indices;
}


QVariantList MarkerTracker::markers() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    QVariantList markers;
    if (inputImage_.empty()) return markers;

    const int width  = inputImage_.cols;
    const int height = inputImage_.rows;

    for (auto&& marker : markers_) {
        QVariantMap o;
        const auto markerPos = toUnit(cv::Point2d(marker.x, marker.y), width, height);
        o.insert("id",         marker.id);
        o.insert("x",          markerPos.x);
        o.insert("y",          markerPos.y);
        o.insert("size",       marker.size / ((width + height) / 2));
        o.insert("angle",      marker.angle);
        o.insert("frameCount", marker.frameCount);
        o.insert("image",      QVariant::fromValue(marker.image));

        QVariantList polygon, edges, indices, patterns;
        for (const auto& vertex : marker.polygon) {
            QVariantMap data;
            const auto vertPos = toUnit(cv::Point2d(vertex.x, vertex.y), width, height);
            const auto vertLocalPos = toLocal(vertPos, markerPos, marker.angle);
            data.insert("x", vertLocalPos.x);
            data.insert("y", vertLocalPos.y);

            polygon.push_back(data);
        }
        for (const auto& edge : marker.edges) {
            if (!edge.activated) continue;

            QVariantMap data;
            data.insert("id", edge.id);

            const auto edgePos = toUnit(cv::Point2d(edge.x, edge.y), width, height);
            const auto edgeLocalPos = toLocal(edgePos, markerPos, marker.angle);
            data.insert("x", edgeLocalPos.x);
            data.insert("y", edgeLocalPos.y);

            QVariantMap direction;
            const auto localDir = rotate(edge.direction, marker.angle);
            direction.insert("x", localDir.x);
            direction.insert("y", localDir.y);
            data.insert("direction", direction);
            edges.push_back(data);
        }
        for (int index : marker.indices) {
            indices.push_back(index);
        }
        for (const auto& pattern : marker.patterns) {
            QVariantMap data;
            QVariantList ids;
            for (const auto& id : pattern.edgeIds) {
                ids.append(id);
            }
            data.insert("ids", ids);
            data.insert("pattern", pattern.pattern);
            patterns.push_back(data);
        }
        o.insert("polygon", polygon);
        o.insert("edges", edges);
        o.insert("indices", indices);
        o.insert("patterns", patterns);

        markers.append(o);
    }

    return markers;
}
