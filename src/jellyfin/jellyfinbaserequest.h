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

#ifndef JELLYFINBASEREQUEST_H
#define JELLYFINBASEREQUEST_H

#include "config.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include "includes/shared_ptr.h"
#include "core/jsonbaserequest.h"
#include "jellyfinservice.h"

class QNetworkReply;
class QNetworkRequest;
class NetworkAccessManager;
class JellyfinService;

class JellyfinBaseRequest : public JsonBaseRequest {
  Q_OBJECT

 public:
  explicit JellyfinBaseRequest(JellyfinService *service, const SharedPtr<NetworkAccessManager> network, QObject *parent = nullptr);

  enum class Type {
    None,
    FavouriteArtists,
    FavouriteAlbums,
    FavouriteSongs,
    SearchArtists,
    SearchAlbums,
    SearchSongs,
  };

 protected:
  QString service_name() const override;
  bool authentication_required() const override;
  bool authenticated() const override;
  bool use_authorization_header() const override;
  QByteArray authorization_header() const override;

  QUrl server_url() const;
  QString access_token() const;
  bool http2() const;
  bool verify_certificate() const;
  bool download_album_covers() const;

  QUrl CreateUrl(const QString &ressource_path) const;

  QNetworkReply *CreateGetRequest(const QString &ressource_path, const ParamList &params);
  QNetworkReply *CreateGetRequest(const QUrl &url);

  JsonObjectResult ParseJsonObject(QNetworkReply *reply);

 private:
  void SetRequestAttributes(QNetworkRequest &network_request) const;

  JellyfinService *service_;
  const SharedPtr<NetworkAccessManager> network_;
};

#endif  // JELLYFINBASEREQUEST_H