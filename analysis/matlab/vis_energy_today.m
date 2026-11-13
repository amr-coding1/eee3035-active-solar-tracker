% VIS_ENERGY_TODAY  Cumulative energy through the latest UTC day.
%
% ThingSpeak "MATLAB Visualization" for the Active Solar Tracker (EEE3035
% Group 8). Shows cumulative Wh: tracked panel, fixed panel, and tracked panel minus the tracker's own use.
% Reads channel 1 ("AST raw", contract v1.5 section 3.1) for the UTC day of
% the newest entry, removes duplicates by seq (section 6.1) and draws one
% static figure. No toolboxes; also runs in GNU Octave 8.
%
% Accessibility: Okabe-Ito colours, a different line style and marker for
% every series, units on both axes, UTC stated, a text note when there is no
% data, and simulated samples (status s=1) announced in the title.

% ------------------------------------------------------------------------
% CONFIGURATION (ThingSpeak: Apps > MATLAB Visualizations > New > Custom)
% ------------------------------------------------------------------------
CH1_ID       = 3578277;  % channel 1 "AST raw" ID (public read)
CH1_READ_KEY = '';       % channel 1 Read API key; '' = public channel (no key sent)
BASE_URL     = 'https://api.thingspeak.com';

% Test overrides (a test harness sets these variables before run(...))
if exist('AST_BASE_URL', 'var'),     BASE_URL = AST_BASE_URL;         end
if exist('AST_CH1_ID', 'var'),       CH1_ID = AST_CH1_ID;             end
if exist('AST_CH1_READ_KEY', 'var'), CH1_READ_KEY = AST_CH1_READ_KEY; end

% Okabe-Ito colour-blind-safe palette
C_BLACK = [0 0 0];          C_ORANGE = [230 159 0] / 255;   C_SKY = [86 180 233] / 255;
C_GREEN = [0 158 115] / 255; C_YELLOW = [240 228 66] / 255; C_BLUE = [0 114 178] / 255;
C_VERMILLION = [213 94 0] / 255; C_PURPLE = [204 121 167] / 255;
FONT_SIZE = 12;

% ------------------------------------------------------------------------
% Read the latest UTC day of channel 1 (this block is the same in every
% vis_*_today.m). Contract sections 3.1, 4, 6.1 and 8.
% Result: day_txt, n (unique samples), n_dup, n_sim, hours (time of day, UTC),
%         V (n-by-8 field values, NaN = missing), state (n-by-1 cell).
% ------------------------------------------------------------------------
EPOCH_DATENUM = 719529;                  % datenum of 1970-01-01
ch1 = num2str(CH1_ID);
key_param = '';
if ~isempty(CH1_READ_KEY)
    key_param = ['api_key=', CH1_READ_KEY, '&'];
end
web_opts = weboptions('Timeout', 15);

% which day: the UTC day of the newest entry (AST_VIS_DAY = 'YYYY-MM-DD' overrides, for tests)
day_no = NaN;                            % days since 1970-01-01
if exist('AST_VIS_DAY', 'var')
    day_no = datenum(AST_VIS_DAY, 'yyyy-mm-dd') - EPOCH_DATENUM;
else
    reply = webread(sprintf('%s/channels/%s/feeds.json?%sresults=1&timezone=Etc/UTC', BASE_URL, ch1, key_param), web_opts);
    if ischar(reply), reply = jsondecode(reply); end        % Octave returns text
    if isstruct(reply) && isfield(reply, 'feeds') && ~isempty(reply.feeds)
        newest = reply.feeds(end);
        if iscell(newest), newest = newest{1}; end
        c = newest.created_at;                               % 'YYYY-MM-DDTHH:MM:SSZ'
        day_no = datenum(str2double(c(1:4)), str2double(c(6:7)), str2double(c(9:10))) - EPOCH_DATENUM;
    end
end

names = {'created_at', 'entry_id', 'status', 'field1', 'field2', 'field3', 'field4', 'field5', 'field6', 'field7', 'field8'};
cols = cell(0, numel(names));
day_txt = '';
if ~isnan(day_no)
    day_txt = datestr(day_no + EPOCH_DATENUM, 'yyyy-mm-dd');
    % two half-day windows, so that up to 16000 entries (duplicates included) can be read
    spans = {'00:00:00', '11:59:59'; '12:00:00', '23:59:59'};
    for w = 1:2
        url = sprintf('%s/channels/%s/feeds.json?%sstart=%s%%20%s&end=%s%%20%s&timezone=Etc/UTC&status=true&results=8000', ...
                      BASE_URL, ch1, key_param, day_txt, spans{w, 1}, day_txt, spans{w, 2});
        reply = webread(url, web_opts);
        if ischar(reply), reply = jsondecode(reply); end
        if ~isstruct(reply) || ~isfield(reply, 'feeds') || isempty(reply.feeds), continue, end
        feeds = reply.feeds;                                 % struct array, or cell array of structs
        m = numel(feeds);
        part = cell(m, numel(names));
        for k = 1:numel(names)
            if isstruct(feeds)
                if isfield(feeds, names{k}), part(:, k) = reshape({feeds.(names{k})}, m, 1); end
            else
                for i = 1:m
                    if isfield(feeds{i}, names{k}), part{i, k} = feeds{i}.(names{k}); end
                end
            end
        end
        cols = [cols; part]; %#ok<AGROW>
    end
end

% parse: time of day, numeric fields, state, simulated flag
ok = cellfun(@(c) ischar(c) && numel(c) >= 19, cols(:, 1));
cols = cols(ok, :);
n_read = size(cols, 1);
hours = zeros(n_read, 1);
if n_read > 0
    T = char(cols(:, 1));
    D = double(T(:, 1:19)) - 48;
    hours = D(:, 12:13) * [10; 1] + D(:, 15:16) * [10; 1] / 60 + D(:, 18:19) * [10; 1] / 3600;
end
V = NaN(n_read, 8);
for k = 1:8
    V(:, k) = str2double(cols(:, k + 3));                    % null or non-numeric -> NaN
end
entry_id = NaN(n_read, 1);
has_id = cellfun(@(c) isnumeric(c) && isscalar(c), cols(:, 2));
entry_id(has_id) = cellfun(@double, cols(has_id, 2));
status = cols(:, 3);
status(~cellfun(@ischar, status)) = {''};
state = regexp(status, '^[^;]*', 'match', 'once');           % text before the first ';'
state(~cellfun(@ischar, state)) = {''};
is_sim = ~cellfun('isempty', regexp(status, ';s=1(?=;|$)(?!.*;s=)', 'match', 'once'));

% de-duplicate (contract 6.1): same seq AND same created_at = a re-sent copy;
% keep the lowest entry_id; same seq at another minute or no seq = keep
seq = fix(V(:, 8));
rank = entry_id;
rank(isnan(rank)) = Inf;
keep = true(n_read, 1);
with_seq = find(~isnan(seq));
if ~isempty(with_seq)
    [~, order] = sortrows([seq(with_seq), hours(with_seq), rank(with_seq)]);
    srt = with_seq(order);
    keep(srt([false; seq(srt(2:end)) == seq(srt(1:end - 1)) & hours(srt(2:end)) == hours(srt(1:end - 1))])) = false;
end
u = find(keep);
[~, order] = sortrows([hours(u), rank(u)]);
u = u(order);
hours = hours(u);  V = V(u, :);  state = state(u);  is_sim = is_sim(u);
n = numel(u);
n_dup = n_read - n;
n_sim = sum(is_sim);

% extra title lines: what the chart is based on
note = {sprintf('%d samples', n)};
if n_dup > 0, note{1} = sprintf('%s (%d duplicates removed)', note{1}, n_dup); end
if n_sim > 0, note{end + 1} = sprintf('SIMULATED DATA: %d of %d samples', n_sim, n); end

% lines are broken where more than 1.5 minutes are missing
gap_after = find(diff(hours) > 1.5 / 60);
hours_ticks = 0:3:24;
hours_labels = {'00:00', '03:00', '06:00', '09:00', '12:00', '15:00', '18:00', '21:00', '24:00'};

figure('Color', 'w');
axes('FontSize', FONT_SIZE);
hold on
NODATA_TITLE = 'Cumulative energy (latest UTC day)';
if n == 0
    % nothing to plot: say so in the picture instead of showing empty axes
    axis([0 1 0 1]);
    axis off
    if isempty(day_txt)
        msg = {'No data', 'Channel 1 has no entries yet.'};
    else
        msg = {'No data', sprintf('Channel 1 has no entries for %s (UTC).', day_txt)};
    end
    text(0.5, 0.5, msg, 'HorizontalAlignment', 'center', 'FontSize', FONT_SIZE + 2);
    title(NODATA_TITLE, 'FontSize', FONT_SIZE - 1, 'Visible', 'on');
else
    % contract 6.2: E = sum(P * 60 s) / 3600 / 1000 [Wh]; a missing power counts as 0
    P = V(:, 1:3);
    P(isnan(P)) = 0;
    E = cumsum(P, 1) * 60 / 3600 / 1000;
    series  = [E(:, 1), E(:, 2), E(:, 1) - E(:, 3)];
    labels  = {sprintf('Tracked panel: %.3g Wh', series(end, 1)), ...
               sprintf('Fixed panel: %.3g Wh', series(end, 2)), ...
               sprintf('Tracked minus own use: %.3g Wh', series(end, 3))};
    colours = {C_BLUE, C_VERMILLION, C_GREEN};
    styles  = {'-', '--', '-.'};
    markers = {'o', 's', '^'};
    h = zeros(1, 3);
    for j = 1:3
        plot(hours, series(:, j), 'LineStyle', styles{j}, 'Color', colours{j}, 'LineWidth', 1.8);
        mk = (1 + (j - 1) * 20):60:n;                        % one marker an hour, staggered between series
        plot(hours(mk), series(mk, j), 'LineStyle', 'none', 'Marker', markers{j}, 'Color', colours{j}, ...
             'MarkerFaceColor', 'w', 'MarkerSize', 6, 'LineWidth', 1.2);
        h(j) = plot(NaN, NaN, 'LineStyle', styles{j}, 'Marker', markers{j}, 'Color', colours{j}, ...
                    'MarkerFaceColor', 'w', 'MarkerSize', 6, 'LineWidth', 1.8);
    end
    if E(end, 2) > 0
        gain_txt = sprintf('Net gain so far %+.1f %% (gross %+.1f %%)', ...
                           100 * (E(end, 1) - E(end, 2) - E(end, 3)) / E(end, 2), 100 * (E(end, 1) - E(end, 2)) / E(end, 2));
    else
        gain_txt = 'Net gain not defined (no fixed-panel energy yet)';
    end
    lo = min(0, min(series(:)));
    top = max(series(:));
    if top <= lo, top = lo + 1; end
    set(gca, 'YLim', [lo, lo + 1.45 * (top - lo)]);          % head room: the legend sits above the curves
    ylabel('Energy since 00:00 UTC (Wh)', 'FontSize', FONT_SIZE);
    legend(h, labels, 'Location', 'northwest', 'FontSize', FONT_SIZE - 1);
    title([{sprintf('Cumulative energy on %s (UTC)', day_txt), gain_txt}, note], 'FontSize', FONT_SIZE - 1);
    set(gca, 'XLim', [0 24], 'XTick', hours_ticks, 'XTickLabel', hours_labels);
    xlabel('Time of day (UTC)', 'FontSize', FONT_SIZE);
    grid on
    box on
end
hold off
