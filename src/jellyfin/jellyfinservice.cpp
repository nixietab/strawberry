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

#include <QVariant>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSslError>
#include <QJsonValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/database.h"
#include "core/networkaccessmanager.h"
#include "core/song.h"
#include "core/settings.h"
#include "core/taskmanager.h"
#include "core/urlhandlers.h"
#include "utilities/randutils.h"
#include "collection/collectionbackend.h"
#include "collection/collectionmodel.h"
#include "jellyfinservice.h"
#include "jellyfinrequest.h"
#include "jellyfinurlhandler.h"
#include "constants/jellyfinsettings.h"

using namespace Qt::Literals::StringLiterals;
using std::make_shared;

const Song::Source JellyfinService::kSource = Song::Source::Jellyfin;
const char *JellyfinService::kClientName = "Strawberry";
const char *JellyfinService::kApiVersion = "1.2.30.0";

namespace {
constexpr char kAuthEndpoint[] = "/Users/AuthenticateByName";

constexpr char kArtistsSongsTable[] = "jellyfin_artists_songs";
constexpr char kAlbumsSongsTable[] = "jellyfin_albums_songs";
constexpr char kSongsTable[] = "jellyfin_songs";

constexpr int kSearchDelayMs = 300;
}  // namespace

JellyfinService::JellyfinService(const SharedPtr<TaskManager> task_manager,
                                 const SharedPtr<Database> database,
                                 const SharedPtr<NetworkAccessManager> network,
                                 const SharedPtr<UrlHandlers> url_handlers,
                                 const SharedPtr<AlbumCoverLoader> albumcover_loader,
                                 QObject *parent)
    : StreamingService(Song::Source::Jellyfin, u"Jellyfin"_s, u"jellyfin"_s, QLatin1String(JellyfinSettings::kSettingsGroup), parent),
      network_(network),
      database_(database),
      task_manager_(task_manager),
      url_handler_(nullptr),
      artists_collection_backend_(nullptr),
      albums_collection_backend_(nullptr),
      songs_collection_backend_(nullptr),
      artists_collection_model_(nullptr),
      albums_collection_model_(nullptr),
      songs_collection_model_(nullptr),
      timer_search_delay_(new QTimer(this)),
      pending_search_id_(0),
      next_pending_search_id_(1),
      pending_search_text_(QString()),
      pending_search_type_(SearchType::Artists),
      search_id_(0),
      http2_(false),
      verify_certificate_(true),
      download_album_covers_(true),
      server_side_scrobbling_(false),
      auto_login_requested_(false) {

  url_handler_ = new JellyfinUrlHandler(this);
  url_handlers->Register(url_handler_);

  // Backends

  artists_collection_backend_ = make_shared<CollectionBackend>();
  artists_collection_backend_->moveToThread(database->thread());
  artists_collection_backend_->Init(database, task_manager, Song::Source::Jellyfin, QLatin1String(kArtistsSongsTable));

  albums_collection_backend_ = make_shared<CollectionBackend>();
  albums_collection_backend_->moveToThread(database->thread());
  albums_collection_backend_->Init(database, task_manager, Song::Source::Jellyfin, QLatin1String(kAlbumsSongsTable));

  songs_collection_backend_ = make_shared<CollectionBackend>();
  songs_collection_backend_->moveToThread(database->thread());
  songs_collection_backend_->Init(database, task_manager, Song::Source::Jellyfin, QLatin1String(kSongsTable));

  // Models

  artists_collection_model_ = new CollectionModel(artists_collection_backend_, albumcover_loader, this);
  albums_collection_model_ = new CollectionModel(albums_collection_backend_, albumcover_loader, this);
  songs_collection_model_ = new CollectionModel(songs_collection_backend_, albumcover_loader, this);

  // Search

  timer_search_delay_->setSingleShot(true);
  timer_search_delay_->setInterval(kSearchDelayMs);
  QObject::connect(timer_search_delay_, &QTimer::timeout, this, &JellyfinService::StartSearch);

  JellyfinService::ReloadSettings();

}

JellyfinService::~JellyfinService() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
  }

}

void JellyfinService::Exit() {

  wait_for_exit_ << &*artists_collection_backend_ << &*albums_collection_backend_ << &*songs_collection_backend_;

  QObject::connect(&*artists_collection_backend_, &CollectionBackend::ExitFinished, this, &JellyfinService::ExitReceived);
  QObject::connect(&*albums_collection_backend_, &CollectionBackend::ExitFinished, this, &JellyfinService::ExitReceived);
  QObject::connect(&*songs_collection_backend_, &CollectionBackend::ExitFinished, this, &JellyfinService::ExitReceived);

  artists_collection_backend_->ExitAsync();
  albums_collection_backend_->ExitAsync();
  songs_collection_backend_->ExitAsync();

}

void JellyfinService::ExitReceived() {

  QObject *obj = sender();
  QObject::disconnect(obj, nullptr, this, nullptr);
  qLog(Debug) << obj << "successfully exited.";
  wait_for_exit_.removeAll(obj);
  if (wait_for_exit_.isEmpty()) Q_EMIT ExitFinished();

}

void JellyfinService::ReloadSettings() {

  Settings s;
  s.beginGroup(JellyfinSettings::kSettingsGroup);

  const bool enabled = s.value(JellyfinSettings::kEnabled, JellyfinSettings::kDefaultEnabled).toBool();

  server_url_ = s.value(JellyfinSettings::kUrl).toUrl();
  username_ = s.value(JellyfinSettings::kUsername).toString();
  QByteArray password = s.value(JellyfinSettings::kPassword).toByteArray();
  if (password.isEmpty()) password_.clear();
  else password_ = QString::fromUtf8(QByteArray::fromBase64(password));

  http2_ = s.value(JellyfinSettings::kHTTP2, JellyfinSettings::kDefaultHTTP2).toBool();
  verify_certificate_ = s.value(JellyfinSettings::kVerifyCertificate, JellyfinSettings::kDefaultVerifyCertificate).toBool();
  download_album_covers_ = s.value(JellyfinSettings::kDownloadAlbumCovers, JellyfinSettings::kDefaultDownloadAlbumCovers).toBool();
  server_side_scrobbling_ = s.value(JellyfinSettings::kServerSideScrobbling, JellyfinSettings::kDefaultServerSideScrobbling).toBool();

  s.endGroup();

  // Automatically log in with the stored credentials so the service is usable on startup
  if (enabled && !authenticated() && server_url_.isValid() && !username_.isEmpty() && !password_.isEmpty()) {
    auto_login_requested_ = true;
    SendPingWithCredentials(server_url_, username_, password_);
  }

}

QString JellyfinService::CreateAuthorizationHeader() const {

  QString device_id = server_url_.toString();
  Settings s;
  s.beginGroup(JellyfinSettings::kSettingsGroup);
  QString stored_device_id = s.value(JellyfinSettings::kDeviceId).toString();
  if (stored_device_id.isEmpty()) {
    stored_device_id = u"strawberry-"_s + Utilities::CryptographicRandomString(16);
    s.setValue(JellyfinSettings::kDeviceId, stored_device_id);
  }
  s.endGroup();

  return u"MediaBrowser Client=\"%1\", Device=\"%1\", DeviceId=\"%2\", Version=\"%3\""_s.arg(QLatin1String(kClientName), stored_device_id, QLatin1String(kApiVersion));

}

void JellyfinService::SendPing() {
  SendPingWithCredentials(server_url_, username_, password_);
}

QUrl JellyfinService::GetStreamUrl(const QString &song_id) const {

  QUrl stream_url = server_url_;
  QString path = stream_url.path();
  if (path.isEmpty()) path = u"/"_s;
  else if (!path.endsWith(u'/')) path.append(u'/');
  stream_url.setPath(path + u"Audio/%1/stream"_s.arg(song_id));

  QUrlQuery query;
  query.addQueryItem(u"static"_s, u"true"_s);
  query.addQueryItem(u"api_key"_s, access_token_);
  stream_url.setQuery(query);

  return stream_url;

}

void JellyfinService::SendPingWithCredentials(QUrl url, const QString &username, const QString &password) {

  QString path = url.path();
  if (path.isEmpty()) path = u"/"_s;
  else if (!path.endsWith(u'/')) path.append(u'/');
  url.setPath(path + QLatin1String(kAuthEndpoint + 1));

  QNetworkReply *reply = CreateAuthenticateRequest(url, username, password);
  if (!reply) return;

  replies_ << reply;
  QObject::connect(reply, &QNetworkReply::sslErrors, this, &JellyfinService::HandleSSLErrors);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, url, username, password]() { HandleAuthReply(reply, url, username, password); });

}

QNetworkReply *JellyfinService::CreateAuthenticateRequest(const QUrl &url, const QString &username, const QString &password) {

  QNetworkRequest network_request(url);
  network_request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
  network_request.setRawHeader("X-Emby-Authorization", CreateAuthorizationHeader().toUtf8());
  network_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  network_request.setAttribute(QNetworkRequest::Http2AllowedAttribute, http2_);
  network_request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
  network_request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);

  if (url.scheme() == "https"_L1 && !verify_certificate_) {
    QSslConfiguration sslconfig = QSslConfiguration::defaultConfiguration();
    sslconfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    network_request.setSslConfiguration(sslconfig);
  }

  QJsonObject json_obj;
  json_obj.insert(u"Username"_s, username);
  json_obj.insert(u"Pw"_s, password);

  return network_->post(network_request, QJsonDocument(json_obj).toJson(QJsonDocument::Compact));

}

void JellyfinService::HandleSSLErrors(const QList<QSslError> &ssl_errors) {

  for (const QSslError &ssl_error : ssl_errors) {
    errors_ += ssl_error.errorString();
  }

}

void JellyfinService::HandleAuthReply(QNetworkReply *reply, const QUrl &url, const QString &username, const QString &password) {

  Q_UNUSED(url);
  Q_UNUSED(username);
  Q_UNUSED(password);

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  if (reply->error() != QNetworkReply::NoError || http_status != 200) {
    if (reply->error() != QNetworkReply::NoError && reply->error() < 200) {
      AuthError(QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
      return;
    }

    const QByteArray data = reply->readAll();
    QJsonParseError parse_error;
    QJsonDocument json_doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error == QJsonParseError::NoError && !json_doc.isEmpty() && json_doc.isObject()) {
      const QJsonObject json_obj = json_doc.object();
      if (json_obj.contains(u"Message"_s)) {
        AuthError(QStringLiteral("%1 (%2)").arg(json_obj.value(u"Message"_s).toString()).arg(http_status));
        return;
      }
    }

    AuthError(QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(http_status));
    return;
  }

  const QByteArray data = reply->readAll();
  QJsonParseError parse_error;
  QJsonDocument json_doc = QJsonDocument::fromJson(data, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || json_doc.isEmpty() || !json_doc.isObject()) {
    AuthError(tr("Missing Json response."));
    return;
  }

  const QJsonObject json_obj = json_doc.object();
  if (!json_obj.contains(u"AccessToken"_s) || !json_obj.contains(u"User"_s)) {
    AuthError(tr("Response is missing AccessToken or User."));
    return;
  }

  const QString access_token = json_obj.value(u"AccessToken"_s).toString();
  const QString user_id = json_obj.value(u"User"_s).toObject().value(u"Id"_s).toString();
  if (access_token.isEmpty() || user_id.isEmpty()) {
    AuthError(tr("Response contains an empty AccessToken or User Id."));
    return;
  }

  SetAuth(access_token, user_id);
  Q_EMIT TestSuccess();
  Q_EMIT TestComplete(true);

  // After an automatic login on startup, load the catalogs right away so the tabs are populated when the user opens them.
  if (auto_login_requested_) {
    auto_login_requested_ = false;
    GetArtists();
    GetAlbums();
    GetSongs();
  }

}

void JellyfinService::AuthError(const QString &error, const QVariant &debug) {

  qLog(Error) << "Jellyfin:" << error << debug;
  Q_EMIT TestFailure(error);
  Q_EMIT TestComplete(false, error);

}

void JellyfinService::GetArtists() {

  if (!authenticated()) {
    Q_EMIT ArtistsResults(SongMap(), tr("Not authenticated with Jellyfin."));
    Q_EMIT OpenSettingsDialog(kSource);
    return;
  }

  artists_request_.reset(new JellyfinRequest(this, network_, JellyfinBaseRequest::Type::FavouriteArtists, this));
  QObject::connect(&*artists_request_, &JellyfinRequest::Results, this, &JellyfinService::ArtistsResultsReceived);
  QObject::connect(&*artists_request_, &JellyfinRequest::UpdateStatus, this, &JellyfinService::ArtistsUpdateStatusReceived);
  QObject::connect(&*artists_request_, &JellyfinRequest::UpdateProgress, this, &JellyfinService::ArtistsUpdateProgressReceived);

  artists_request_->Process();

}

void JellyfinService::ResetArtistsRequest() {
  artists_request_.reset();
}

void JellyfinService::ArtistsResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);
  Q_EMIT ArtistsResults(songs, error);
  if (!songs.isEmpty()) artists_collection_backend_->UpdateSongsBySongIDAsync(songs);
  ResetArtistsRequest();

}

void JellyfinService::ArtistsUpdateStatusReceived(const int id, const QString &text) {
  Q_UNUSED(id);
  Q_EMIT ArtistsUpdateStatus(text);
}

void JellyfinService::ArtistsUpdateProgressReceived(const int id, const int progress) {
  Q_UNUSED(id);
  Q_EMIT ArtistsUpdateProgress(progress);
}

void JellyfinService::GetAlbums() {

  if (!authenticated()) {
    Q_EMIT AlbumsResults(SongMap(), tr("Not authenticated with Jellyfin."));
    Q_EMIT OpenSettingsDialog(kSource);
    return;
  }

  albums_request_.reset(new JellyfinRequest(this, network_, JellyfinBaseRequest::Type::FavouriteAlbums, this));
  QObject::connect(&*albums_request_, &JellyfinRequest::Results, this, &JellyfinService::AlbumsResultsReceived);
  QObject::connect(&*albums_request_, &JellyfinRequest::UpdateStatus, this, &JellyfinService::AlbumsUpdateStatusReceived);
  QObject::connect(&*albums_request_, &JellyfinRequest::UpdateProgress, this, &JellyfinService::AlbumsUpdateProgressReceived);

  albums_request_->Process();

}

void JellyfinService::ResetAlbumsRequest() {
  albums_request_.reset();
}

void JellyfinService::AlbumsResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);
  Q_EMIT AlbumsResults(songs, error);
  if (!songs.isEmpty()) albums_collection_backend_->UpdateSongsBySongIDAsync(songs);
  ResetAlbumsRequest();

}

void JellyfinService::AlbumsUpdateStatusReceived(const int id, const QString &text) {
  Q_UNUSED(id);
  Q_EMIT AlbumsUpdateStatus(text);
}

void JellyfinService::AlbumsUpdateProgressReceived(const int id, const int progress) {
  Q_UNUSED(id);
  Q_EMIT AlbumsUpdateProgress(progress);
}

void JellyfinService::GetSongs() {

  if (!authenticated()) {
    Q_EMIT SongsResults(SongMap(), tr("Not authenticated with Jellyfin."));
    Q_EMIT OpenSettingsDialog(kSource);
    return;
  }

  songs_request_.reset(new JellyfinRequest(this, network_, JellyfinBaseRequest::Type::FavouriteSongs, this));
  QObject::connect(&*songs_request_, &JellyfinRequest::Results, this, &JellyfinService::SongsResultsReceived);
  QObject::connect(&*songs_request_, &JellyfinRequest::UpdateStatus, this, &JellyfinService::SongsUpdateStatusReceived);
  QObject::connect(&*songs_request_, &JellyfinRequest::UpdateProgress, this, &JellyfinService::SongsUpdateProgressReceived);

  songs_request_->Process();

}

void JellyfinService::ResetSongsRequest() {
  songs_request_.reset();
}

void JellyfinService::SongsResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);
  Q_EMIT SongsResults(songs, error);
  if (!songs.isEmpty()) songs_collection_backend_->UpdateSongsBySongIDAsync(songs);
  ResetSongsRequest();

}

void JellyfinService::SongsUpdateStatusReceived(const int id, const QString &text) {
  Q_UNUSED(id);
  Q_EMIT SongsUpdateStatus(text);
}

void JellyfinService::SongsUpdateProgressReceived(const int id, const int progress) {
  Q_UNUSED(id);
  Q_EMIT SongsUpdateProgress(progress);
}

int JellyfinService::Search(const QString &text, const SearchType type) {

  pending_search_id_ = next_pending_search_id_++;
  pending_search_text_ = text;
  pending_search_type_ = type;

  if (text.isEmpty()) {
    timer_search_delay_->stop();
    return pending_search_id_;
  }
  timer_search_delay_->start();

  return pending_search_id_;

}

void JellyfinService::CancelSearch() {
  search_request_.reset();
}

void JellyfinService::StartSearch() {

  if (!authenticated()) {
    Q_EMIT SearchResults(pending_search_id_, SongMap(), tr("Not authenticated with Jellyfin."));
    Q_EMIT OpenSettingsDialog(kSource);
    return;
  }

  search_id_ = pending_search_id_;
  search_text_ = pending_search_text_;

  if (search_request_) {
    search_request_.reset();
  }

  JellyfinBaseRequest::Type query_type = JellyfinBaseRequest::Type::None;

  switch (pending_search_type_) {
    case SearchType::Artists:
      query_type = JellyfinBaseRequest::Type::SearchArtists;
      break;
    case SearchType::Albums:
      query_type = JellyfinBaseRequest::Type::SearchAlbums;
      break;
    case SearchType::Songs:
      query_type = JellyfinBaseRequest::Type::SearchSongs;
      break;
    default:
      return;
  }

  search_request_.reset(new JellyfinRequest(this, network_, query_type, this));
  QObject::connect(&*search_request_, &JellyfinRequest::Results, this, &JellyfinService::SearchResultsReceived);
  QObject::connect(&*search_request_, &JellyfinRequest::UpdateStatus, this, &JellyfinService::SearchUpdateStatus);
  QObject::connect(&*search_request_, &JellyfinRequest::UpdateProgress, this, &JellyfinService::SearchUpdateProgress);

  search_request_->Search(search_id_, search_text_);
  search_request_->Process();

}

void JellyfinService::SearchResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_EMIT SearchResults(id, songs, error);
  search_request_.reset();

}

void JellyfinService::SearchUpdateStatus(const int id, const QString &text) {
  Q_EMIT StreamingService::SearchUpdateStatus(id, text);
}

void JellyfinService::SearchUpdateProgress(const int id, const int progress) {
  Q_EMIT StreamingService::SearchUpdateProgress(id, progress);
}