% AST_NIGHTLY  Nightly summary job for the Active Solar Tracker (EEE3035 Group 8).
%
% ThingSpeak "MATLAB Analysis" script, run by a TimeControl every night at
% 00:20 UTC. Reads channel 1 ("AST raw", one entry per minute), summarises the
% last NDAYS complete UTC days and writes one row per day to channel 2
% ("AST results") in a single bulk update.
%
% Implements docs/CONTRACT.md v1.5:
%   3.2  channel-2 row format          6.1  de-duplication by seq
%   6.2  energy and gain               6.3  pointing error
%   6.4  completeness                  8    read API (windows, 8000 limit)
% The same numbers come from analysis/py/ast_metrics.py (reference) and from
% the dashboard; analysis/tests/run_tests.sh checks this script against the
% reference in GNU Octave.
%
% Runs unchanged in MATLAB (no toolboxes) and in GNU Octave 8, so it uses only
% numeric POSIX seconds, char arrays and cell arrays (no datetime, timetable
% or string objects, no local functions, no file I/O).
% NOTE for Octave only: its built-in webwrite cannot post a JSON body; add
% analysis/tests/octave_shims to the path (the test script does this).

% ------------------------------------------------------------------------
% CONFIGURATION - paste the channel-2 Write API key after pasting into ThingSpeak
% ------------------------------------------------------------------------
CH1_ID        = 3578277;                % channel 1 "AST raw" ID (public read)
CH1_READ_KEY  = '';                     % channel 1 Read API key; '' = public channel (no key sent)
CH2_ID        = 3578278;                % channel 2 "AST results" ID
CH2_WRITE_KEY = 'PASTE_CH2_WRITE_KEY';  % channel 2 Write API key (never commit the real key)
BASE_URL      = 'https://api.thingspeak.com';
NDAYS         = 9;                      % complete UTC days rewritten every night (8-day device buffer + 1)
DRY_RUN       = 'auto';                 % 'auto': dry run until a real key is pasted; true/false to force

% ------------------------------------------------------------------------
% Test overrides: a test harness sets AST_* variables, then run('ast_nightly.m')
% ------------------------------------------------------------------------
if exist('AST_BASE_URL', 'var'),      BASE_URL = AST_BASE_URL;           end
if exist('AST_CH1_ID', 'var'),        CH1_ID = AST_CH1_ID;               end
if exist('AST_CH1_READ_KEY', 'var'),  CH1_READ_KEY = AST_CH1_READ_KEY;   end
if exist('AST_CH2_ID', 'var'),        CH2_ID = AST_CH2_ID;               end
if exist('AST_CH2_WRITE_KEY', 'var'), CH2_WRITE_KEY = AST_CH2_WRITE_KEY; end
if exist('AST_NDAYS', 'var'),         NDAYS = AST_NDAYS;                 end
if exist('AST_DRY_RUN', 'var'),       DRY_RUN = AST_DRY_RUN;             end

% ------------------------------------------------------------------------
% Constants (contract sections 3, 6 and 8)
% ------------------------------------------------------------------------
AST_VERSION    = '1.5';          % written as v=... in every channel-2 status
MAX_RESULTS    = 8000;           % ThingSpeak returns at most 8000 entries per read
WINDOW_DAYS    = 4;              % read window (4 days = 5760 one-minute entries)
EXPECTED_DAY   = 1440;           % minute slots in a complete UTC day
WH_PER_SAMPLE  = 60 / 3600 / 1000;   % one 60 s sample of 1 mW = this many Wh
EPOCH_DATENUM  = 719529;         % datenum of 1970-01-01
HTTP_TIMEOUT_S = 15;
NUM_PATTERN    = '^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$';
TIME_PATTERN   = '^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(Z|[+-]\d{2}:\d{2})?$';

t_start = tic;
ch1 = num2str(CH1_ID);
ch2 = num2str(CH2_ID);
if strcmp(ch1, '0') || strcmp(ch2, '0')
    error('AST:config', 'Set CH1_ID and CH2_ID at the top of the script.');
end
no_key = isempty(CH2_WRITE_KEY) || strcmp(CH2_WRITE_KEY, 'PASTE_CH2_WRITE_KEY');
if ischar(DRY_RUN)                       % 'auto': write as soon as a real key is pasted
    DRY_RUN = no_key;
    if no_key
        fprintf('No channel-2 write key yet: dry run. Paste the key into CH2_WRITE_KEY to start writing.\n');
    end
end
if ~DRY_RUN && no_key
    error('AST:config', 'Set CH2_WRITE_KEY at the top of the script, or set DRY_RUN = true to test without writing.');
end

% "now" in POSIX seconds, UTC
if exist('AST_NOW_UTC', 'var')
    now_utc = AST_NOW_UTC;
elseif exist('OCTAVE_VERSION', 'builtin')
    now_utc = time();
else
    now_utc = posixtime(datetime('now', 'TimeZone', 'UTC'));
end
now_utc = floor(now_utc);

% POSIX seconds -> 'YYYY-MM-DD' and 'YYYY-MM-DD HH:MM:SS' (UTC), exact integer arithmetic
fmt_date = @(t) datestr(floor(t / 86400) + EPOCH_DATENUM, 'yyyy-mm-dd');
fmt_time = @(t) sprintf('%s %02d:%02d:%02d', datestr(floor(t / 86400) + EPOCH_DATENUM, 'yyyy-mm-dd'), ...
                        floor(mod(t, 86400) / 3600), floor(mod(t, 3600) / 60), mod(t, 60));

% The NDAYS complete UTC days before now: day numbers (days since 1970-01-01)
today_no  = floor(now_utc / 86400);
first_no  = today_no - NDAYS;
t_first   = first_no * 86400;            % 00:00:00 of the oldest day
t_end     = today_no * 86400;            % 00:00:00 today (exclusive end)
fprintf('AST nightly, contract v%s. Now %s UTC. Days %s to %s.\n', AST_VERSION, fmt_time(now_utc), ...
        fmt_date(t_first), fmt_date(t_end - 86400));

% ------------------------------------------------------------------------
% 1. Read channel 1 in windows (section 8)
% ------------------------------------------------------------------------
% Each row of 'queue' is one window [start end], both inclusive, POSIX seconds.
w_start = (t_first : WINDOW_DAYS * 86400 : t_end - 1)';
queue = [w_start, min(w_start + WINDOW_DAYS * 86400, t_end) - 1];

raw_time   = cell(0, 1);     % created_at text
raw_id     = zeros(0, 1);    % entry_id
raw_field  = cell(0, 8);     % field1..field8 as received (text, [] for null)
raw_status = cell(0, 1);     % status text
n_requests = 0;
n_splits   = 0;
n_truncated = 0;
read_opts = weboptions('Timeout', HTTP_TIMEOUT_S);
key_param = '';                          % a public channel is read without api_key
if ~isempty(CH1_READ_KEY)
    key_param = ['api_key=', CH1_READ_KEY, '&'];
end

while ~isempty(queue)
    w = queue(1, :);
    queue(1, :) = [];
    url = sprintf('%s/channels/%s/feeds.json?%sstart=%s&end=%s&timezone=Etc/UTC&status=true&results=%d', ...
                  BASE_URL, ch1, key_param, strrep(fmt_time(w(1)), ' ', '%20'), ...
                  strrep(fmt_time(w(2)), ' ', '%20'), MAX_RESULTS);
    try
        reply = webread(url, read_opts);
    catch read_err
        error('AST:read', 'Reading channel %s failed (%s). Check CH1_ID and CH1_READ_KEY.', ch1, read_err.message);
    end
    n_requests = n_requests + 1;
    if ischar(reply)                     % Octave returns the text, MATLAB decodes JSON itself
        reply = jsondecode(reply);
    end
    if ~isstruct(reply) || ~isfield(reply, 'feeds')
        error('AST:read', 'Channel %s did not return a feed list. Check CH1_ID and CH1_READ_KEY.', ch1);
    end
    feeds = reply.feeds;
    n = numel(feeds);

    % A full reply means older entries of the window may be cut off: split it.
    if n >= MAX_RESULTS
        if w(2) - w(1) >= 120
            mid = w(1) + floor((w(2) - w(1) + 1) / 120) * 60;     % whole-minute boundary
            queue = [w(1), mid - 1; mid, w(2); queue];
            n_splits = n_splits + 1;
            continue
        end
        n_truncated = n_truncated + 1;   % cannot split a one-minute window any further
    end
    if n == 0
        continue
    end

    % jsondecode gives a struct array when every feed has the same keys and a
    % cell array of structs otherwise; a null value is []. Handle both.
    c_time = cell(n, 1);  c_id = cell(n, 1);  c_status = cell(n, 1);  c_field = cell(n, 8);
    if isstruct(feeds)
        if isfield(feeds, 'created_at'), c_time = reshape({feeds.created_at}, n, 1); end
        if isfield(feeds, 'entry_id'),   c_id = reshape({feeds.entry_id}, n, 1);     end
        if isfield(feeds, 'status'),     c_status = reshape({feeds.status}, n, 1);   end
        for k = 1:8
            name = sprintf('field%d', k);
            if isfield(feeds, name), c_field(:, k) = reshape({feeds.(name)}, n, 1); end
        end
    else
        for i = 1:n
            f = feeds{i};
            if isfield(f, 'created_at'), c_time{i} = f.created_at; end
            if isfield(f, 'entry_id'),   c_id{i} = f.entry_id;     end
            if isfield(f, 'status'),     c_status{i} = f.status;   end
            for k = 1:8
                name = sprintf('field%d', k);
                if isfield(f, name), c_field{i, k} = f.(name); end
            end
        end
    end
    id = NaN(n, 1);
    has_id = cellfun(@(v) isnumeric(v) && isscalar(v), c_id);
    id(has_id) = cellfun(@double, c_id(has_id));

    raw_time   = [raw_time; c_time];       %#ok<AGROW> (at most a few windows)
    raw_id     = [raw_id; id];             %#ok<AGROW>
    raw_field  = [raw_field; c_field];     %#ok<AGROW>
    raw_status = [raw_status; c_status];   %#ok<AGROW>
end
n_read = numel(raw_time);

% ------------------------------------------------------------------------
% 2. Parse (sections 3.1 and 4)
% ------------------------------------------------------------------------
% created_at 'YYYY-MM-DDTHH:MM:SSZ' (or with a +hh:mm offset) -> POSIX seconds
t = NaN(n_read, 1);
is_text = cellfun(@ischar, raw_time);
raw_time(~is_text) = {''};
ok = ~cellfun('isempty', regexp(raw_time, TIME_PATTERN, 'match', 'once'));
if any(ok)
    C = char(raw_time(ok));
    C(:, end + 1:25) = ' ';
    D = double(C) - 48;                                     % digit characters -> 0..9
    yy = D(:, 1:4) * [1000; 100; 10; 1];
    mo = D(:, 6:7) * [10; 1];
    dd = D(:, 9:10) * [10; 1];
    secs = D(:, 12:13) * [36000; 3600] + D(:, 15:16) * [600; 60] + D(:, 18:19) * [10; 1];
    sgn = double(C(:, 20) == '+') - double(C(:, 20) == '-');     % 0 for 'Z'
    offset = zeros(size(sgn));
    has_off = sgn ~= 0;
    offset(has_off) = sgn(has_off) .* (D(has_off, 21:22) * [36000; 3600] + D(has_off, 24:25) * [600; 60]);
    t(ok) = (datenum(yy, mo, dd) - EPOCH_DATENUM) * 86400 + secs - offset;
end

% field1..field8: text -> number, null / empty / not a plain number -> NaN
V = NaN(n_read, 8);
for k = 1:8
    col = raw_field(:, k);
    x = NaN(n_read, 1);
    is_text = cellfun(@ischar, col);
    if any(is_text)
        s = strtrim(col(is_text));
        v = NaN(numel(s), 1);
        good = ~cellfun('isempty', regexp(s, NUM_PATTERN, 'match', 'once'));
        v(good) = str2double(s(good));
        x(is_text) = v;
    end
    is_num = cellfun(@(q) isnumeric(q) && isscalar(q), col);      % tolerated: JSON numbers
    x(is_num) = cellfun(@double, col(is_num));
    x(~isfinite(x)) = NaN;
    V(:, k) = x;
end

% status 'TRK;r=12;p=A;e=0;s=1': the state is the text before the first ';',
% s=1 marks a simulated sample (the last s= wins, as in the other readers)
is_text = cellfun(@ischar, raw_status);
raw_status(~is_text) = {''};
state = regexp(raw_status, '^[^;]*', 'match', 'once');
state(~cellfun(@ischar, state)) = {''};
is_tracking = ismember(state, {'TRK', 'MOV', 'HLD'});               % states that count for pointing error
is_sim = ~cellfun('isempty', regexp(raw_status, ';s=1(?=;|$)(?!.*;s=)', 'match', 'once'));

% keep entries with a readable time inside the summarised days
in_range = ~isnan(t) & t >= t_first & t < t_end;
n_bad_time = sum(isnan(t));
t = t(in_range);  V = V(in_range, :);  id = raw_id(in_range);
is_tracking = is_tracking(in_range);  is_sim = is_sim(in_range);
n_in = numel(t);
day = floor(t / 86400) - first_no + 1;                              % 1..NDAYS, 1 = oldest

fprintf('Channel %s: %d request(s), %d window split(s), %d entries read, %d in range, %d without a readable time.\n', ...
        ch1, n_requests, n_splits, n_read, n_in, n_bad_time);
if n_truncated > 0
    fprintf('WARNING: %d one-minute window(s) still returned %d entries; some duplicates were not read.\n', ...
            n_truncated, MAX_RESULTS);
end

% ------------------------------------------------------------------------
% 3. De-duplicate inside each day (section 6.1)
% ------------------------------------------------------------------------
% A duplicate has the same seq AND the same created_at (a re-sent record).
% Keep the first received copy (lowest entry_id). The same seq at another
% minute is a different sample (seq restarted). No seq = always kept.
seq = fix(V(:, 8));
id_rank = id;
id_rank(isnan(id_rank)) = Inf;
keep = true(n_in, 1);
with_seq = find(~isnan(seq));
if ~isempty(with_seq)
    [~, order] = sortrows([day(with_seq), seq(with_seq), t(with_seq), id_rank(with_seq)]);
    srt = with_seq(order);
    same_as_prev = [false; day(srt(2:end)) == day(srt(1:end - 1)) & seq(srt(2:end)) == seq(srt(1:end - 1)) ...
                    & t(srt(2:end)) == t(srt(1:end - 1))];
    keep(srt(same_as_prev)) = false;
end
n_received = accumarray(day, 1, [NDAYS, 1]);

% unique entries in time order (entry_id breaks ties), as in the reference
u = find(keep);
[~, order] = sortrows([t(u), id_rank(u)]);
u = u(order);
du = day(u);

% ------------------------------------------------------------------------
% 4. Metrics per day (sections 6.2 to 6.4)
% ------------------------------------------------------------------------
P = V(u, 1:3);
P(isnan(P)) = 0;                                        % a null power counts as 0
E_trk = accumarray(du, P(:, 1), [NDAYS, 1]) * WH_PER_SAMPLE;      % Wh
E_fix = accumarray(du, P(:, 2), [NDAYS, 1]) * WH_PER_SAMPLE;
E_use = accumarray(du, P(:, 3), [NDAYS, 1]) * WH_PER_SAMPLE;
N_unique = accumarray(du, 1, [NDAYS, 1]);
n_sim = accumarray(du, double(is_sim(u)), [NDAYS, 1]);
n_dup = n_received - N_unique;

gross = NaN(NDAYS, 1);
net = NaN(NDAYS, 1);
has_fix = E_fix > 0;                                    % gains undefined when E_fix <= 0
gross(has_fix) = 100 * (E_trk(has_fix) - E_fix(has_fix)) ./ E_fix(has_fix);
net(has_fix) = 100 * (E_trk(has_fix) - E_fix(has_fix) - E_use(has_fix)) ./ E_fix(has_fix);

err = V(u, 5) - V(u, 4);                                % theta_sun - theta_panel
counts = is_tracking(u) & ~isnan(err);
n_point = accumarray(du(counts), 1, [NDAYS, 1]);
mae = accumarray(du(counts), abs(err(counts)), [NDAYS, 1]) ./ n_point;
mae(n_point == 0) = NaN;                                % no pointing samples

complete = 100 * N_unique / EXPECTED_DAY;               % every summarised day is a past day

% ------------------------------------------------------------------------
% 5. Build the bulk update (section 3.2) and write it
% ------------------------------------------------------------------------
% Energies: 6 significant figures; percentages and angles: 4 dp; N: integer.
% Tie guard: readers round these texts again (3 s.f. / 1 dp, section 6.5). A
% written value that sits exactly on such a rounding boundary (digits '500')
% is moved one last-digit step back towards the unrounded value, so rounding
% twice can never differ from rounding once.
updates = cell(NDAYS, 1);
fprintf('date        n     N     dup  sim   E_trk Wh    E_fix Wh    E_use Wh    gross %%   net %%     MAE deg  complete %%\n');
for d = 1:NDAYS
    energy = [E_trk(d), E_fix(d), E_use(d)];
    energy_txt = cell(1, 3);
    for j = 1:3
        a = abs(energy(j));
        if a == 0
            energy_txt{j} = '0';
            continue
        end
        e_str = sprintf('%.5e', a);                     % 'd.ddddde+XX'
        digits = [e_str(1), e_str(3:7)];
        ex = sscanf(e_str(9:end), '%d');
        for pass = 1:2
            if ex >= 5
                txt = [digits, repmat('0', 1, ex - 5)];
            elseif ex >= 0
                txt = [digits(1:ex + 1), '.', digits(ex + 2:end)];
            else
                txt = ['0.', repmat('0', 1, -ex - 1), digits];
            end
            if pass == 2 || ~strcmp(digits(4:6), '500')
                break
            end
            written = str2double(txt);                  % tie guard
            if a < written
                digits(4:6) = '499';
            elseif a > written
                digits(4:6) = '501';
            end
        end
        if energy(j) < 0
            txt = ['-', txt];                           %#ok<AGROW>
        end
        energy_txt{j} = txt;
    end

    pct = [gross(d), net(d), mae(d), complete(d)];      % field4..field7
    pct_txt = cell(1, 4);
    for j = 1:4
        if isnan(pct(j))
            pct_txt{j} = '';                            % omitted from the update
            continue
        end
        txt = sprintf('%.4f', pct(j));
        if strcmp(txt(end - 2:end), '500')              % tie guard
            written = str2double(txt);
            if pct(j) < written
                txt = sprintf('%.4f', written - 0.0001);
            elseif pct(j) > written
                txt = sprintf('%.4f', written + 0.0001);
            end
        end
        if strcmp(txt, '-0.0000')
            txt = '0.0000';
        end
        pct_txt{j} = txt;
    end

    date_txt = fmt_date(t_first + (d - 1) * 86400);
    parts = {sprintf('"created_at":"%s +0000"', fmt_time(now_utc + d - 1)), ...
             sprintf('"field1":"%s"', energy_txt{1}), ...
             sprintf('"field2":"%s"', energy_txt{2}), ...
             sprintf('"field3":"%s"', energy_txt{3})};
    for j = 1:4
        if ~isempty(pct_txt{j})
            parts{end + 1} = sprintf('"field%d":"%s"', j + 3, pct_txt{j}); %#ok<SAGROW>
        end
    end
    parts{end + 1} = sprintf('"field8":"%d"', N_unique(d)); %#ok<SAGROW>
    parts{end + 1} = sprintf('"status":"period=day;date=%s;v=%s;ns=%d;nd=%d"', ...
                             date_txt, AST_VERSION, n_sim(d), n_dup(d)); %#ok<SAGROW>
    updates{d} = ['{', strjoin(parts, ','), '}'];

    shown = pct_txt;
    shown(cellfun('isempty', shown)) = {'-'};
    fprintf('%s  %-5d %-5d %-4d %-5d %-11s %-11s %-11s %-9s %-9s %-8s %s\n', date_txt, n_received(d), ...
            N_unique(d), n_dup(d), n_sim(d), energy_txt{1}, energy_txt{2}, energy_txt{3}, shown{1}, shown{2}, ...
            shown{3}, shown{4});
end

body = ['{"write_api_key":"', CH2_WRITE_KEY, '","updates":[', strjoin(updates', ','), ']}'];
if DRY_RUN
    % the key is not part of what is reported: only the size of the body
    fprintf('DRY RUN: nothing written. The bulk update for channel %s would hold %d row(s), JSON body %d characters.\n', ...
            ch2, NDAYS, numel(body));
    fprintf('Elapsed %.1f s.\n', toc(t_start));
else
    write_url = sprintf('%s/channels/%s/bulk_update.json', BASE_URL, ch2);
    write_opts = weboptions('MediaType', 'application/json', 'Timeout', HTTP_TIMEOUT_S);
    try
        answer = webwrite(write_url, body, write_opts);     % char data is sent as it is
    catch write_err
        error('AST:write', 'Writing channel %s failed (%s). Check CH2_ID and CH2_WRITE_KEY.', ch2, write_err.message);
    end
    if ischar(answer)
        try
            answer = jsondecode(answer);
        catch
            answer = struct('success', false);
        end
    end
    if ~(isstruct(answer) && isfield(answer, 'success') && isequal(answer.success, true))
        error('AST:write', 'Channel %s did not confirm the bulk update.', ch2);
    end
    fprintf('Channel %s: wrote %d row(s) in one bulk update. Elapsed %.1f s.\n', ch2, NDAYS, toc(t_start));
end
