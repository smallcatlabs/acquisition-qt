// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#pragma once

constexpr int kEditThreadTimeout = 300000; // 5 minutes, pathofexile.com can be very slow
constexpr int kImgurUploadTimeout = 10000; // 10s
constexpr int kPoeApiTimeout = 10000;
// A request to the OAuth token endpoint. Longer than kPoeApiTimeout: the
// server may honour a refresh and rotate the refresh token, and an answer
// abandoned after that leaves a refresh token stored that no longer works.
constexpr int kOAuthTokenTimeout = 30000;
