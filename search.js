// SPDX-FileCopyrightText: 2021 GNOME Foundation
//
// SPDX-License-Identifier: Apache-2.0 OR GPL-3.0-or-later

window.buildIndex = function (rawSearchIndex) {
    let request = new XMLHttpRequest();
    request.open('GET', rawSearchIndex, true);
    request.onreadystatechange = function() {
        if (request.readyState === XMLHttpRequest.DONE) {
            var status = request.status;

            if (status === 0 || (status >= 200 && status < 400)) {
                var searchIndex = JSON.parse(request.responseText);
                window.initSearch(searchIndex);
            }
        }
    }
    request.send(null);
}
