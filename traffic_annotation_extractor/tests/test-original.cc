// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/traffic_annotation/network_traffic_annotation.h"

#include "dummy_classes.h"

// This file provides samples for testing traffic_annotation_extractor clang
// tool.
namespace {

net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("id1", R"(
        semantics {
          sender: "sender1"
          description: "desc1"
          trigger: "trigger1"
          data: "data1"
          destination: GOOGLE_OWNED_SERVICE
        }
        policy {
          cookies_allowed: NO
          setting: "setting1"
          chrome_policy {
            SpellCheckServiceEnabled {
                SpellCheckServiceEnabled: false
            }
          }
        }
        comments: "comment1")");
}

void Sample1() {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation("id2", "completing_id2", R"(
        semantics {
          sender: "sender2"
          description: "desc2"
          trigger: "trigger2"
          data: "data2"
          destination: WEBSITE
        })");

  net::NetworkTrafficAnnotationTag completed_traffic_annotation =
      net::CompleteNetworkTrafficAnnotation("id3", partial_traffic_annotation,
                                            R"(
        policy {
          cookies_allowed: YES
          cookie_store: "user"
          setting: "setting3"
          chrome_policy {
            SpellCheckServiceEnabled {
                SpellCheckServiceEnabled: false
            }
          }
        }
        comments: "comment3")");

  net::NetworkTrafficAnnotationTag completed_branch_traffic_annotation =
      net::BranchedCompleteNetworkTrafficAnnotation(
          "id4", "branch4", partial_traffic_annotation, R"(
        policy {
          cookies_allowed: YES
          cookie_store: "user"
          setting: "setting4"
          policy_exception_justification: "justification"
        })");
}

void Sample2() {
  net::URLFetcherDelegate* delegate = nullptr;
  net::URLFetcher::Create(GURL(), net::URLFetcher::RequestType::TEST_VALUE,
                          delegate);

  net::URLFetcher::Create(0, GURL(), net::URLFetcher::RequestType::TEST_VALUE,
                          delegate);

  net::URLFetcher::Create(GURL(), net::URLFetcher::RequestType::TEST_VALUE,
                          delegate, kTrafficAnnotation);

  net::URLFetcher::Create(0, GURL(), net::URLFetcher::RequestType::TEST_VALUE,
                          delegate, NO_TRAFFIC_ANNOTATION_YET);
}

void Sample3() {
  net::URLRequest::Delegate* delegate = nullptr;
  net::URLRequestContext context;

  context.CreateRequest(GURL(), net::RequestPriority::TEST_VALUE, delegate);
  context.CreateRequest(GURL(), net::RequestPriority::TEST_VALUE, delegate,
                        kTrafficAnnotation);
}