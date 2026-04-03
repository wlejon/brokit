// Test: TreeWalker and NodeFilter
assert(typeof TreeWalker === 'function', 'TreeWalker exists');
assert(typeof NodeFilter === 'object', 'NodeFilter exists');

// NodeFilter constants
assertEqual(NodeFilter.FILTER_ACCEPT, 1, 'FILTER_ACCEPT');
assertEqual(NodeFilter.FILTER_REJECT, 2, 'FILTER_REJECT');
assertEqual(NodeFilter.FILTER_SKIP, 3, 'FILTER_SKIP');
assertEqual(NodeFilter.SHOW_ALL, 0xFFFFFFFF, 'SHOW_ALL');
assertEqual(NodeFilter.SHOW_ELEMENT, 0x1, 'SHOW_ELEMENT');
assertEqual(NodeFilter.SHOW_TEXT, 0x4, 'SHOW_TEXT');
assertEqual(NodeFilter.SHOW_COMMENT, 0x80, 'SHOW_COMMENT');

// Build a mock DOM tree to test traversal:
//   root (element, nodeType=1)
//     |- text1 (text, nodeType=3)
//     |- child1 (element, nodeType=1)
//     |    |- text2 (text, nodeType=3)
//     |- child2 (element, nodeType=1)
//     |    |- grandchild (element, nodeType=1)
//     |- comment (comment, nodeType=8)

function mockNode(nodeType, name, children) {
    var node = {
        nodeType: nodeType,
        nodeName: name,
        childNodes: children || [],
        parentNode: null
    };
    for (var i = 0; i < node.childNodes.length; i++) {
        node.childNodes[i].parentNode = node;
    }
    return node;
}

var text1 = mockNode(3, '#text', []);
var text2 = mockNode(3, '#text', []);
var grandchild = mockNode(1, 'SPAN', []);
var child1 = mockNode(1, 'DIV', [text2]);
var child2 = mockNode(1, 'P', [grandchild]);
var comment = mockNode(8, '#comment', []);
var root = mockNode(1, 'BODY', [text1, child1, child2, comment]);

// Use assert(a === b) for node identity checks — assertEqual uses JSON.stringify
// as fallback which fails on circular parentNode refs.

// TreeWalker: SHOW_ALL
var tw = new TreeWalker(root, NodeFilter.SHOW_ALL);
assert(tw.currentNode === root, 'initial currentNode is root');

var first = tw.firstChild();
assert(first === text1, 'firstChild is text1');

var next = tw.nextSibling();
assert(next === child1, 'nextSibling is child1');

next = tw.nextSibling();
assert(next === child2, 'nextSibling is child2');

next = tw.firstChild();
assert(next === grandchild, 'firstChild of child2 is grandchild');

// parentNode
var p = tw.parentNode();
assert(p === child2, 'parentNode from grandchild is child2');

// TreeWalker: SHOW_ELEMENT only
var tw2 = new TreeWalker(root, NodeFilter.SHOW_ELEMENT);
var elements = [];
var n = tw2.firstChild();
while (n) {
    elements.push(n.nodeName);
    n = tw2.nextNode();
}
assert(elements.indexOf('DIV') !== -1, 'SHOW_ELEMENT visits DIV');
assert(elements.indexOf('P') !== -1, 'SHOW_ELEMENT visits P');
assert(elements.indexOf('SPAN') !== -1, 'SHOW_ELEMENT visits SPAN');
assert(elements.indexOf('#text') === -1, 'SHOW_ELEMENT skips text');
assert(elements.indexOf('#comment') === -1, 'SHOW_ELEMENT skips comment');

// TreeWalker: SHOW_TEXT only
var tw3 = new TreeWalker(root, NodeFilter.SHOW_TEXT);
var texts = [];
n = tw3.nextNode();
while (n) {
    texts.push(n);
    n = tw3.nextNode();
}
assertEqual(texts.length, 2, 'SHOW_TEXT finds 2 text nodes');
assert(texts[0] === text1, 'first text is text1');
assert(texts[1] === text2, 'second text is text2');

// TreeWalker: custom filter
var tw4 = new TreeWalker(root, NodeFilter.SHOW_ELEMENT, function(node) {
    return node.nodeName === 'SPAN' ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_SKIP;
});
var found = tw4.nextNode();
assert(found === grandchild, 'custom filter finds SPAN');
var noMore = tw4.nextNode();
assert(noMore === null, 'no more nodes after SPAN');

// __brokit_install_createTreeWalker exists
assert(typeof __brokit_install_createTreeWalker === 'function', 'install helper exists');
