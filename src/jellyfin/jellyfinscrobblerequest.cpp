/*
 * Strawberry Music Player
 * Copyright 2026, Strawberry Music Player contributors
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QDateTime>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>

#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "jellyfinservice.h"
#include "jellyfinbaserequest.h"
#include "jellyfinscrobblerequest.h"

using namespace Qt::Literals::StringLiterals;

namespace {
constexpr int kMaxConcurrentScrobbleRequests = 3;
constexpr qint64 kTicksPerMsec = 10000;
}

JellyfinScrobbleRequest::JellyfinScrobbleRequest(JellyfinService *service, const SharedPtr<NetworkAccessManager> network, QObject *parent)
    : JellyfinBaseRequest(service, network, parent),
      scrobble_requests_active_(0) {}

void JellyfinScrobbleRequest::CreateScrobbleRequest(const QString &song_id, const bool submission, const QDateTime &start_time) {

  Request request;
  request.type = submission ? Request::Type::Stopped : Request::Type::Start;
  request.song_id = song_id;
  request.time = start_time;
  scrobble_requests_queue_.enqueue(request);
  if (scrobble_requests_active_ < kMaxConcurrentScrobbleRequests) FlushScrobbleRequests();

}

void JellyfinScrobbleRequest::CreatePlaybackProgressRequest(const QString &song_id, const QDateTime &start_time) {

  Request request;
  request.type = Request::Type::Progress;
  request.song_id = song_id;
  request.time = start_time;
  scrobble_requests_queue_.enqueue(request);
  if (scrobble_requests_active_ < kMaxConcurrentScrobbleRequests) FlushScrobbleRequests();

}

void JellyfinScrobbleRequest::FlushScrobbleRequests() {

  while (!scrobble_requests_queue_.isEmpty() && scrobble_requests_active_ < kMaxConcurrentScrobbleRequests) {

    Request request = scrobble_requests_queue_.dequeue();
    ++scrobble_requests_active_;

    QJsonObject json_object;
    json_object.insert(u"ItemId"_s, request.song_id);

    QString ressource_path;
    switch (request.type) {
      case Request::Type::Progress:
        // PlaybackProgressInfo: the current position, in ticks (100 ns units), to keep the session alive on the server.
        json_object.insert(u"PositionTicks"_s, request.time.msecsTo(QDateTime::currentDateTime()) * kTicksPerMsec);
        ressource_path = u"Sessions/Playing/Progress"_s;
        break;
      case Request::Type::Stopped:
        // PlaybackStopInfo: the position, in ticks (100 ns units), where playback stopped.
        json_object.insert(u"PositionTicks"_s, request.time.msecsTo(QDateTime::currentDateTime()) * kTicksPerMsec);
        json_object.insert(u"Failed"_s, false);
        ressource_path = u"Sessions/Playing/Stopped"_s;
        break;
      case Request::Type::Start:
        // PlaybackStartInfo: a fresh playback starts at position 0.
        json_object.insert(u"PositionTicks"_s, 0);
        ressource_path = u"Sessions/Playing"_s;
        break;
    }

    QNetworkReply *reply = CreatePostRequest(ressource_path, json_object);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { ScrobbleReplyReceived(reply); });

  }

}

void JellyfinScrobbleRequest::ScrobbleReplyReceived(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  --scrobble_requests_active_;

  // The playback-reporting endpoints answer with 204 No Content on success.
  const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  qLog(Debug) << "JellyfinScrobbleRequest: Received" << http_status << "for" << reply->url().toString()
              << "error:" << reply->errorString();

  if (reply->error() != QNetworkReply::NoError || http_status != 204) {
    Error(QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(http_status));
    FinishCheck();
    return;
  }

  FinishCheck();

}

void JellyfinScrobbleRequest::FinishCheck() {

  if (!scrobble_requests_queue_.isEmpty() && scrobble_requests_active_ < kMaxConcurrentScrobbleRequests) FlushScrobbleRequests();

}

void JellyfinScrobbleRequest::Error(const QString &error, const QVariant &debug) {

  if (!error.isEmpty()) {
    qLog(Error) << "JellyfinScrobbleRequest:" << error;
    errors_ << error;
  }
  if (debug.isValid()) qLog(Debug) << debug;

  FinishCheck();

}