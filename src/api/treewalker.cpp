#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>

namespace brokit::api {

// TreeWalker and NodeFilter as a pure JS polyfill.
// Operates on JS node objects via standard DOM properties
// (childNodes, firstChild, nextSibling, parentNode, nodeType).
// No C++ DOM dependency — works with any DOM implementation that
// exposes these properties on JS node wrappers.

void installTreeWalker(JSContext* ctx)
{
    const char* polyfill = R"JS(
(function() {
    // NodeFilter constants
    var NodeFilter = {
        FILTER_ACCEPT: 1,
        FILTER_REJECT: 2,
        FILTER_SKIP: 3,
        SHOW_ALL: 0xFFFFFFFF,
        SHOW_ELEMENT: 0x1,
        SHOW_ATTRIBUTE: 0x2,
        SHOW_TEXT: 0x4,
        SHOW_CDATA_SECTION: 0x8,
        SHOW_ENTITY_REFERENCE: 0x10,
        SHOW_ENTITY: 0x20,
        SHOW_PROCESSING_INSTRUCTION: 0x40,
        SHOW_COMMENT: 0x80,
        SHOW_DOCUMENT: 0x100,
        SHOW_DOCUMENT_TYPE: 0x200,
        SHOW_DOCUMENT_FRAGMENT: 0x400,
        SHOW_NOTATION: 0x800
    };

    // Map nodeType to whatToShow bit
    function nodeTypeToBit(nodeType) {
        // nodeType: 1=Element, 3=Text, 8=Comment, 9=Document, 11=DocumentFragment
        switch (nodeType) {
            case 1: return NodeFilter.SHOW_ELEMENT;
            case 2: return NodeFilter.SHOW_ATTRIBUTE;
            case 3: return NodeFilter.SHOW_TEXT;
            case 4: return NodeFilter.SHOW_CDATA_SECTION;
            case 7: return NodeFilter.SHOW_PROCESSING_INSTRUCTION;
            case 8: return NodeFilter.SHOW_COMMENT;
            case 9: return NodeFilter.SHOW_DOCUMENT;
            case 10: return NodeFilter.SHOW_DOCUMENT_TYPE;
            case 11: return NodeFilter.SHOW_DOCUMENT_FRAGMENT;
            default: return 0;
        }
    }

    function TreeWalker(root, whatToShow, filter) {
        this.root = root;
        this.whatToShow = whatToShow || NodeFilter.SHOW_ALL;
        this.filter = filter || null;
        this.currentNode = root;
    }

    TreeWalker.prototype._acceptNode = function(node) {
        // whatToShow check
        var bit = nodeTypeToBit(node.nodeType);
        if (!(this.whatToShow & bit)) return NodeFilter.FILTER_SKIP;

        // Custom filter
        if (this.filter) {
            if (typeof this.filter === 'function') return this.filter(node);
            if (typeof this.filter.acceptNode === 'function') return this.filter.acceptNode(node);
        }

        return NodeFilter.FILTER_ACCEPT;
    };

    TreeWalker.prototype._firstChildHelper = function(node) {
        // childNodes is the standard property
        var children = node.childNodes;
        if (!children) return null;
        for (var i = 0; i < children.length; i++) {
            var child = children[i];
            var result = this._acceptNode(child);
            if (result === NodeFilter.FILTER_ACCEPT) return child;
            if (result === NodeFilter.FILTER_SKIP) {
                var inner = this._firstChildHelper(child);
                if (inner) return inner;
            }
            // FILTER_REJECT: skip entire subtree
        }
        return null;
    };

    TreeWalker.prototype._lastChildHelper = function(node) {
        var children = node.childNodes;
        if (!children) return null;
        for (var i = children.length - 1; i >= 0; i--) {
            var child = children[i];
            var result = this._acceptNode(child);
            if (result === NodeFilter.FILTER_ACCEPT) return child;
            if (result === NodeFilter.FILTER_SKIP) {
                var inner = this._lastChildHelper(child);
                if (inner) return inner;
            }
        }
        return null;
    };

    TreeWalker.prototype.parentNode = function() {
        var node = this.currentNode;
        while (node && node !== this.root) {
            node = node.parentNode;
            if (node && this._acceptNode(node) === NodeFilter.FILTER_ACCEPT) {
                this.currentNode = node;
                return node;
            }
        }
        return null;
    };

    TreeWalker.prototype.firstChild = function() {
        var found = this._firstChildHelper(this.currentNode);
        if (found) this.currentNode = found;
        return found;
    };

    TreeWalker.prototype.lastChild = function() {
        var found = this._lastChildHelper(this.currentNode);
        if (found) this.currentNode = found;
        return found;
    };

    TreeWalker.prototype.nextSibling = function() {
        return this._siblingHelper(false);
    };

    TreeWalker.prototype.previousSibling = function() {
        return this._siblingHelper(true);
    };

    TreeWalker.prototype._siblingHelper = function(reverse) {
        var node = this.currentNode;
        if (node === this.root) return null;
        while (true) {
            var parent = node.parentNode;
            if (!parent) return null;
            var siblings = parent.childNodes;
            if (!siblings) return null;
            var idx = -1;
            for (var i = 0; i < siblings.length; i++) {
                if (siblings[i] === node) { idx = i; break; }
            }
            var step = reverse ? -1 : 1;
            for (var i = idx + step; i >= 0 && i < siblings.length; i += step) {
                var sib = siblings[i];
                var result = this._acceptNode(sib);
                if (result === NodeFilter.FILTER_ACCEPT) {
                    this.currentNode = sib;
                    return sib;
                }
                if (result === NodeFilter.FILTER_SKIP) {
                    var inner = reverse ? this._lastChildHelper(sib) : this._firstChildHelper(sib);
                    if (inner) { this.currentNode = inner; return inner; }
                }
            }
            // Move up — but don't escape above root
            if (parent === this.root) return null;
            node = parent;
            if (this._acceptNode(node) === NodeFilter.FILTER_ACCEPT) return null;
        }
    };

    TreeWalker.prototype.nextNode = function() {
        var node = this.currentNode;
        // Try children first
        var child = this._firstChildHelper(node);
        if (child) { this.currentNode = child; return child; }
        // Try siblings and ancestors' siblings
        while (node && node !== this.root) {
            var parent = node.parentNode;
            if (!parent) return null;
            var siblings = parent.childNodes;
            if (siblings) {
                var idx = -1;
                for (var i = 0; i < siblings.length; i++) {
                    if (siblings[i] === node) { idx = i; break; }
                }
                for (var i = idx + 1; i < siblings.length; i++) {
                    var sib = siblings[i];
                    var result = this._acceptNode(sib);
                    if (result === NodeFilter.FILTER_ACCEPT) {
                        this.currentNode = sib;
                        return sib;
                    }
                    if (result === NodeFilter.FILTER_SKIP) {
                        var inner = this._firstChildHelper(sib);
                        if (inner) { this.currentNode = inner; return inner; }
                    }
                }
            }
            node = parent;
        }
        return null;
    };

    TreeWalker.prototype.previousNode = function() {
        var node = this.currentNode;
        if (node === this.root) return null;
        // Try previous siblings' deepest last child
        var parent = node.parentNode;
        if (!parent) return null;
        var siblings = parent.childNodes;
        if (siblings) {
            var idx = -1;
            for (var i = 0; i < siblings.length; i++) {
                if (siblings[i] === node) { idx = i; break; }
            }
            for (var i = idx - 1; i >= 0; i--) {
                var sib = siblings[i];
                // Go as deep as possible
                var deep = sib;
                while (true) {
                    var last = this._lastChildHelper(deep);
                    if (!last) break;
                    deep = last;
                }
                var result = this._acceptNode(deep);
                if (result === NodeFilter.FILTER_ACCEPT) {
                    this.currentNode = deep;
                    return deep;
                }
            }
        }
        // Return parent
        if (parent !== this.root && this._acceptNode(parent) === NodeFilter.FILTER_ACCEPT) {
            this.currentNode = parent;
            return parent;
        }
        return null;
    };

    // Install on document (if exists) and as standalone constructor
    globalThis.NodeFilter = NodeFilter;
    globalThis.TreeWalker = TreeWalker;

    // document.createTreeWalker — will work once a document object exists
    if (typeof globalThis.__brokit_install_createTreeWalker === 'undefined') {
        globalThis.__brokit_install_createTreeWalker = function(doc) {
            doc.createTreeWalker = function(root, whatToShow, filter) {
                return new TreeWalker(root, whatToShow, filter);
            };
        };
    }
})();
)JS";

    JSValue r = JS_Eval(ctx, polyfill, strlen(polyfill), "<treewalker>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
