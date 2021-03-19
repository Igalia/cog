// SPDX-FileCopyrightText: 2021 GNOME Foundation
//
// SPDX-License-Identifier: Apache-2.0 OR GPL-3.0-or-later

// eslint-disable-next-line no-unused-vars
function hasClass(elem, className) {
    return elem && elem.classList && elem.classList.contains(className);
}

// eslint-disable-next-line no-unused-vars
function addClass(elem, className) {
    if (!elem || !elem.classList) {
        return;
    }
    elem.classList.add(className);
}

// eslint-disable-next-line no-unused-vars
function removeClass(elem, className) {
    if (!elem || !elem.classList) {
        return;
    }
    elem.classList.remove(className);
}

function insertAfter(newNode, referenceNode) {
    referenceNode.parentNode.insertBefore(newNode, referenceNode.nextSibling);
}

function onEach(arr, func, reversed) {
    if (arr && arr.length > 0 && func) {
        var length = arr.length;
        var i;
        if (reversed !== true) {
            for (i = 0; i < length; ++i) {
                if (func(arr[i]) === true) {
                    return true;
                }
            }
        } else {
            for (i = length - 1; i >= 0; --i) {
                if (func(arr[i]) === true) {
                    return true;
                }
            }
        }
    }
    return false;
}

function onEachLazy(lazyArray, func, reversed) {
    return onEach(
        Array.prototype.slice.call(lazyArray),
        func,
        reversed);
}

// eslint-disable-next-line no-unused-vars
function hasOwnProperty(obj, property) {
    return Object.prototype.hasOwnProperty.call(obj, property);
}

function getSearchElement() {
    return document.getElementById("search");
}

function getSearchInput() {
    return document.getElementsByClassName("search-input")[0];
}

function getQueryStringParams() {
    var params = {};
    window.location.search.substring(1).split('&').
        map(function(s) {
            var pair = s.split('=');
            params[decodeURIComponent(pair[0])] =
                typeof pair[1] === 'undefined' ? null : decodeURIComponent(pair[1]);
        });
    return params;
}

function getQuery(query) {
    var query_lower = query.toLowerCase(),
        query_split = query_lower.split('+');

    query_split = query_split.filter(function(chunk) { return chunk !== ""; });

    return {
        raw: query,
        terms: query_split,
        user: query_split.join(' '),
    }
}

window.initSearch = function(searchIndex) {
        var search_input = getSearchInput();
        var params = getQueryStringParams();

        var searchSymbols = searchIndex["symbols"];
        var searchTerms = searchIndex["terms"];

        if (search_input.value === "") {
            search_input.value === params.q || "";
        }

        function runQuery(query) {
            function getDocumentFromId(id) {
                if (typeof id === "number") {
                    return searchSymbols[id];
                }
                return null;
            }

            function getLinkForDocument(doc) {
                switch (doc.type) {
                    case "alias":
                        return "alias." + doc.name + ".html";
                    case "bitfield":
                        return "flags." + doc.name + ".html";
                    case "callback":
                        return "callback." + doc.name + ".html";
                    case "class":
                        return "class." + doc.name + ".html";
                    case "class_method":
                        return "class_method." + doc.type_name + "." + doc.name + ".html";
                    case "constant":
                        return "const." + doc.name + ".html";
                    case "ctor":
                        return "ctor." + doc.type_name + "." + doc.name + ".html";
                    case "domain":
                        return "error." + doc.name + ".html";
                    case "enum":
                        return "enum." + doc.name + ".html";
                    case "function":
                        return "func." + doc.name + ".html";
                    case "function_macro":
                        return "func." + doc.name + ".html";
                    case "interface":
                        return "iface." + doc.name + ".html";
                    case "method":
                        return "method." + doc.type_name + "." + doc.name + ".html";
                    case "property":
                        return "property." + doc.type_name + "." + doc.name + ".html";
                    case "record":
                        return "struct." + doc.name + ".html";
                    case "type_func":
                        return "type_func." + doc.type_name + "." + doc.name + ".html";
                    case "union":
                        return "union." + doc.name + ".html";
                    case "vfunc":
                        return "vfunc." + doc.type_name + "." + doc.name + ".html";
                }

                return null;
            }

            function getTextForDocument(doc) {
                switch (doc.type) {
                    case "alias":
                    case "bitfield":
                    case "class":
                    case "domain":
                    case "enum":
                    case "interface":
                    case "record":
                    case "union":
                        return doc.ctype;
                    case "class_method":
                    case "constant":
                    case "ctor":
                    case "function":
                    case "function_macro":
                    case "method":
                    case "type_func":
                        return doc.ident;
                    case "property":
                        return doc.type_name + ":" + doc.name;
                    case "signal":
                        return doc.type_name + "::" + doc.name;
                    case "vfunc":
                        return doc.type_name + "." + doc.name;
                    case "callback":
                        return doc.name;
                }

                return null;
            }

            const PREDICATE_ALL = 0;
            const PREDICATE_ANY = 1;

            function uniqueResults(arrays) {
                var unique = [];

                for (var it = 0; it < arrays.length; it++) {
                    arrays[it].forEach(function(e) {
                        if (unique.findIndex((doc) => doc.type === e.type && doc.name === e.name) == -1) {
                            unique.push(e);
                        }
                    });
                }

                return unique;
            }

            function mergeArrays(arrays, predicate) {
                if (predicate === PREDICATE_ANY) {
                    return uniqueResults(arrays);
                }

                if (predicate === PREDICATE_ALL) {
                    // find the smallest array
                    var smallest = 1000000;
                    for (var it = 0; it < arrays.length; it++) {
                        if (arrays[it].length < smallest) {
                            smallest = it;
                        }
                    }

                    if (smallest > arrays.length) {
                        return [];
                    }

                    var results = [];

                    // check for elements in every array
                    arrays[smallest].forEach(function(e) {
                        var found = [];
                        for (var it = 0; it < arrays.length; it++) {
                            if (it == smallest) {
                                found.push(smallest);
                            } else {
                                if (arrays[it].findIndex((doc) => doc.type === e.type && doc.name === e.name) != -1) {
                                    found.push(it);
                                }
                            }
                        }
                        if (found.length === arrays.length) {
                            results.push(e);
                        }
                    });

                    return results;
                }

                return [];
            }

            var termIndex = 0;
            var results = [];

            query.terms.forEach(function(term) {
                if (searchTerms.hasOwnProperty(term)) {
                    var docs = searchTerms[term];

                    results[termIndex] = [];

                    docs.forEach(function(id) {
                        var doc = getDocumentFromId(id);

                        if (doc !== null) {
                            var res = {
                                name: doc.name,
                                type: doc.type,
                                text: getTextForDocument(doc),
                                href: getLinkForDocument(doc),
                            };

                            results[termIndex].push(res);
                        }
                    });

                    termIndex += 1;
                }
            });

            if (query.terms.length == 1) {
                return {
                    all: results[0],
                    any: results[0],
                }
            }

            return {
                all: mergeArrays(results, PREDICATE_ALL),
                any: mergeArrays(results, PREDICATE_ANY),
            }
        }

        function showSearchResults(search) {
            if (search === null || typeof search === 'undefined') {
                search = getSearchElement();
            }

            addClass(main, "hidden");
            removeClass(search, "hidden");
        }

        function hideSearchResults(search) {
            if (search === null || typeof search === 'undefined') {
                search = getSearchElement();
            }

            addClass(search, "hidden");
            removeClass(search, "hidden");
        }

        function addResults(results) {
            var output = "";

            if (results.length > 0) {
                output += "<table class=\"results\">" +
                          "<tr><th>Type</th><th>Name</th></tr>";

                results.forEach(function(item) {
                    output += "<tr>" +
                              "<td class=\"result " + item.type + "\">[" + item.type + "]</td>" +
                              "<td><a href=\"" + item.href + "\"><code>" + item.text + "</code></a></td>" +
                              "</tr>";
                });

                output += "</table>";
            } else {
                output = "No results found.";
            }

            return output;
        }

        function showResults(query, results) {
            var search = getSearchElement();

            var output = "<h1>Results for &quot;" + query.user + "&quot; (" + results.all.length + ")</h1>" +
                         "<div id=\"search-results\">" +
                         addResults(results.all) +
                         "</div>";

            search.innerHTML = output;
            showSearchResults(search);
        }

        function search() {
            var query = getQuery(getQueryStringParams().q);

            if (search_input.value === "" && query) {
                if (query.terms.length === 0) {
                    return;
                }

                search_input.value = query.user;
            }

            window.title = "Results for: " + query.user;

            showResults(query, runQuery(query));
        }

        window.onpageshow = function() {
            var query = getQuery(getQueryStringParams().q);
            if (search_input.value === "" && query) {
                search_input.value = query.user;
            }
            search();
        };

        if (getQueryStringParams().q) {
            search();
        }
};

window.addEventListener("load", function() {
    "use strict;"

    var main = document.getElementById("main");
    var btnToTop = document.getElementById("btn-to-top");

    var searchInput = getSearchInput();

    function labelForToggleButton(isCollapsed) {
        if (isCollapsed) {
            return "+";
        }
        return "\u2212";
    }

    function createToggle(isCollapsed) {
        var toggle = document.createElement("a");
        toggle.href = "javascript:void(0)";
        toggle.className = "collapse-toggle";
        toggle.innerHTML = "[<span class=\"inner\">"
                         + labelForToggleButton(isCollapsed)
                         + "</span>]";

        return toggle;
    }

    function toggleClicked() {
        if (hasClass(this, "collapsed")) {
            removeClass(this, "collapsed");
            this.innerHTML = "[<span class=\"inner\">"
                           + labelForToggleButton(false)
                           + "</span>]";
            onEachLazy(this.parentNode.getElementsByClassName("docblock"), function(e) {
                removeClass(e, "hidden");
            });
        } else {
            addClass(this, "collapsed");
            this.innerHTML = "[<span class=\"inner\">"
                           + labelForToggleButton(true)
                           + "</span>]";
            onEachLazy(this.parentNode.getElementsByClassName("docblock"), function(e) {
                addClass(e, "hidden");
            });
        }
    }

    onEachLazy(document.getElementsByClassName("toggle-wrapper"), function(e) {
        collapsedByDefault = hasClass(e, "default-hide");
        var toggle = createToggle(collapsedByDefault);
        toggle.onclick = toggleClicked;
        e.insertBefore(toggle, e.firstChild);
        if (collapsedByDefault) {
            addClass(toggle, "collapsed");
            onEachLazy(e.getElementsByClassName("docblock"), function(d) {
                addClass(d, "hidden");
            });
        }
    });

    function scrollBackTop(e) {
        e.preventDefault();
        window.scroll({
            top: 0,
            behavior: 'smooth',
        });
    }

    function toggleScrollButton() {
        if (window.scrollY < 400) {
            addClass(btnToTop, "hidden");
        } else {
            removeClass(btnToTop, "hidden");
        }
    }

    function resolveNamespaceLink(namespace) {
        try {
            let urlMap = new Map(baseURLs);
            if (urlMap.has(namespace)) {
                return urlMap.get(namespace);
            }
            return '';
        } catch (e) {
            return '';
        }
    }

    window.onscroll = toggleScrollButton;
    btnToTop.onclick = scrollBackTop;

    onEachLazy(document.getElementsByClassName("external"), function(e) {
        if (e.tagName == "A" && e.dataset.hasOwnProperty('namespace')) {
            var data_namespace = e.dataset.namespace
            var data_link = e.dataset.link
            var base_url = resolveNamespaceLink(data_namespace)
            if (base_url !== '') {
                e.href = base_url + data_link;
            } else {
                e.title = "No reference to the " + data_namespace + " namespace";
            }
        }
    });

    if (window.buildIndex) {
        window.buildIndex('index.json');
    }
}, false);
