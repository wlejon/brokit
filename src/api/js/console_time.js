(function(c) {
    var timers = {};
    c.time = function(label) {
        label = label || 'default';
        timers[label] = Date.now();
    };
    c.timeEnd = function(label) {
        label = label || 'default';
        var start = timers[label];
        if (start === undefined) {
            c.warn('Timer "' + label + '" does not exist');
            return;
        }
        var elapsed = Date.now() - start;
        delete timers[label];
        c.log(label + ': ' + elapsed + 'ms');
    };
    c.timeLog = function(label) {
        label = label || 'default';
        var start = timers[label];
        if (start === undefined) {
            c.warn('Timer "' + label + '" does not exist');
            return;
        }
        c.log(label + ': ' + (Date.now() - start) + 'ms');
    };
})(console);
