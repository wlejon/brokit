(function() {
    if (typeof globalThis.navigator !== 'undefined') return;

    globalThis.navigator = {
        userAgent: 'brokit/1.0',
        language: 'en-US',
        languages: ['en-US', 'en'],
        onLine: true,
        hardwareConcurrency: 1,
        maxTouchPoints: 0,
        cookieEnabled: false,
        platform: typeof process !== 'undefined' ? process.platform : 'unknown',
        vendor: '',
        product: 'Gecko',
        productSub: '20030107'
    };
})();
