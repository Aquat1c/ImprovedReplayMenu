IMPROVED REPLAY MENU FOR EFZ  (version 1.1.1)
=============================================
日本語の説明は英語の説明の後にあります。

This mod replaces the replay menu in Eternal Fighter Zero. It adds folders, an
options menu and Japanese text. During replays it adds rewind, fast forward,
round skipping and a frame counter.

Install
-------
1. Close the game.
2. Install EFZ Mod Manager if you do not already have it.
3. Copy the mod folder into your EFZ folder:

   <EFZ folder>\mods\ImprovedReplayMenu\

4. The folder should contain:

   ImprovedReplayMenu.dll
   system\

5. Open EfzModManager.ini beside efz.exe and add this line under [Mods], above
   EnglishTranslation if you use it:

   ImprovedReplayMenu=1

6. Start the game the way you normally do with EFZ Mod Manager.

To uninstall, set ImprovedReplayMenu=0 or delete the mod folder.

Replay menu
-----------
Open Replay from the game's menu.

  Up / Down     Move through the list
  Left / Right  Change page
  Confirm       Play a replay or open a folder
  Cancel        Go back to the parent folder, or leave the menu from the top folder
  D button      Open or close Options
  C button      Start recording (same as Record Replay)

Options:
  Record Replay   Start a new match and record it
  Sort/Filter     Sort and filter the list (see below)
  Play Folder     Play every replay in the folder in order
  Refresh Folder  Read the folder again
  Scrolling BG    Turn the moving background on or off
  Language        Auto, English or Japanese
  Frame counter   Show or hide the frame counter during replays
  Rewind keys     Change the keys below
  About           Shown at the bottom: mod name, version and build date

Sorting and filtering
---------------------
Open Options, then Sort/Filter. Left and Right change a value. Filters apply to
every folder until you clear them or close the game.

  Sort       Newest, oldest, player name or largest file
  Character 1, Character 2  One character: replays with it on either side.
             Two characters: only that matchup, whichever side each is on
  Player     Replays with this player on either side; choose from the players in
             the folder you are in
  Clear filters  Press Confirm to remove both filters

- Above the list you will see what is filtered and how many replays match.
- Play Folder plays only the replays that match.
- Player names and dates come from file names like
  20250321-0215 Name (Character) vs Name (Character).rep
  Files with the game's own names, like data1-1.rep, have no player names, so a
  player filter hides them. They sort by the file's date, and character filters
  still work on them.

Keys during a replay
--------------------
  Key         Tap                         Hold
  -           One frame back              Rewind, getting faster
  P           One frame forward           Fast forward, getting faster
  Backspace   Restart the current round
  \           Skip to the next round
  H           Show or hide the frame counter

- A tap leaves the replay paused. Press Revival's pause key to continue.
- When you release a held key, the replay goes back to how it was: still
  playing or still paused.
- In about the first second of a round, Backspace goes to the round before
  instead, so pressing it twice quickly goes back one round.
- The first time you skip a round the game has to find it, so "FF" shows briefly.
- These defaults follow your keyboard layout, so the keys may differ on your
  keyboard. Check them in Options > Rewind keys, where you can change them.
- P replaces Revival's next-frame key during replays only. Nothing changes in
  other game modes.

Frame counter: shows the current frame and the total, like 1234/5678, at the
bottom of the screen. Short messages can appear beside it. Hiding the counter
hides these too:

  REW         Rewinding
  FF          Fast forwarding or looking for the next round
  TAKEOVER    You are controlling a character
  REPLAY      Back to the recorded replay
  NO CKPT     There is no saved point to go back to from here
  LAST ROUND  There is no later round
  ROUND 1     Already at the start of the first round
  F8 ON       Turn off the game's F8 pause first

Takeover
--------
Revival lets you take control of a character by pressing a direction or game
button during a replay. It listens to Player 1's controls unless you press
Revival's SwitchPlayers key (Num 0 by default). This needs a replay recorded with
Revival.

With this mod, you no longer have to save first to get back to the replay:

- Tap - (or Revival's replay back and forward keys, Num - and Num + by default)
  to return to the moment you took over. The replay then plays on normally.
  Holding - keeps rewinding from there.
- Backspace restarts the round. \ skips to the next round.
- The replay also comes back by itself shortly after a KO, when the match ends,
  or when the recording runs out.
- In takeover, P steps one frame and pauses. A key your character plays with does
  not work as a rewind key.

Notes
-----
- Rewind, fast forward and round skipping need EfzRevival 1.02e, 1.02f, 1.02g,
  1.02h, 1.02i or 1.02j. On any other version these keys turn themselves off and
  the Rewind keys option is hidden.
- The menu reads replays recorded by vanilla EFZ and by old and new Revival
  versions. Playback itself is done by the game and Revival.
- You can rewind as far back as the replay has played, up to about 19,000 frames.
- To turn these keys off, add this to ImprovedReplayMenu.ini beside the DLL
  (create the file if it is not there):

  [Rewind]
  Enabled=0

- Your settings are saved in ImprovedReplayMenu.ini beside the DLL.
- If a key does nothing, another mod may be using it. Pick another key in
  Options > Rewind keys.


============================================================================
日本語
============================================================================

エターナルファイターゼロのリプレイメニューを置き換えるModです。フォルダ表示、
オプションメニュー、日本語表示を追加します。リプレイ再生中は、巻き戻し、早送り、
ラウンドスキップ、フレームカウンターが使えます。

インストール
------------
1. ゲームを終了します。
2. EFZ Mod Manager が入っていない場合は、先にインストールします。
3. Modのフォルダを EFZ のフォルダにコピーします。

   <EFZのフォルダ>\mods\ImprovedReplayMenu\

4. フォルダの中身は次のとおりです。

   ImprovedReplayMenu.dll
   system\

5. efz.exe と同じ場所にある EfzModManager.ini を開き、[Mods] の中に次の行を
   追加します。EnglishTranslation を使っている場合は、その行より上に追加してください。

   ImprovedReplayMenu=1

6. EFZ Mod Manager を使うときと同じ方法でゲームを起動します。

アンインストールするには、ImprovedReplayMenu=0 にするか、Modのフォルダを
削除してください。

リプレイメニュー
----------------
ゲームのメニューからリプレイを開きます。

  上 / 下        リストを移動
  左 / 右        ページを切り替え
  決定           リプレイを再生、またはフォルダを開く
  キャンセル     親フォルダに戻る (最上位のフォルダではメニューを閉じる)
  Dボタン        オプションを開く / 閉じる
  Cボタン        録画を開始 (「リプレイを録画」と同じ)

オプション:
  リプレイを録画        新しい対戦を始めて録画
  並び替え/絞り込み     リストの並び替えと絞り込み (下記参照)
  フォルダを連続再生    フォルダ内のリプレイを順番に再生
  フォルダを再読み込み  フォルダを読み直す
  背景スクロール        背景の動きをオン / オフ
  言語                  自動 / English / 日本語
  フレームカウンター    リプレイ再生中のカウンターを表示 / 非表示
  巻き戻しキー          下記のキーを変更
  バージョン情報        下部に表示 (Mod名、バージョン、ビルド日時)

並び替えと絞り込み
------------------
オプションの「並び替え/絞り込み」を開きます。左右キーで値を変更します。絞り込みは、
解除するかゲームを終了するまで、すべてのフォルダに適用されます。

  並び順          新しい順 / 古い順 / プレイヤー名 / サイズ順
  キャラクター1、キャラクター2  1人: どちらかの側がそのキャラクターのリプレイ。
                  2人: その組み合わせのリプレイのみ (左右は問いません)
  プレイヤー      どちらかの側がこのプレイヤーのリプレイ (今開いているフォルダ内の
                  プレイヤーから選びます)
  絞り込みを解除  決定で両方の絞り込みを解除

- リストの上に、絞り込みの内容と該当するリプレイの数が表示されます。
- 「フォルダを連続再生」では、該当するリプレイだけが再生されます。
- プレイヤー名と日付は、次のようなファイル名から取得します。
  20250321-0215 名前 (キャラクター) vs 名前 (キャラクター).rep
  data1-1.rep のようなゲーム標準の名前のファイルにはプレイヤー名がないため、
  プレイヤーで絞り込むと表示されません。これらはファイルの日付で並び替えられ、
  キャラクターでの絞り込みは使えます。

リプレイ再生中のキー
--------------------
  キー        短押し                長押し
  -           1フレーム戻る         巻き戻し (だんだん速くなります)
  P           1フレーム進む         早送り (だんだん速くなります)
  Backspace   現在のラウンドの先頭へ
  \           次のラウンドへスキップ
  H           フレームカウンターを表示 / 非表示

- 短押しすると一時停止します。再開するには Revival の一時停止キーを押してください。
- 長押しをやめると、押す前の状態 (再生中または一時停止中) に戻ります。
- ラウンド開始から約1秒の間は、Backspace で1つ前のラウンドに戻ります。そのため、
  素早く2回押すと1つ前のラウンドに戻れます。
- 初めてスキップするときはラウンドを探すため、少しの間「FF」と表示されます。
- 初期設定のキーはキーボード配列に合わせて決まるため、上記と異なる場合があります。
  オプションの「巻き戻しキー」で確認と変更ができます。
- P はリプレイ再生中に限り Revival のコマ送りキーを置き換えます。他のゲームモードには
  影響しません。

フレームカウンター: 現在のフレームと総フレーム数を 1234/5678 のように画面下部に
表示します。横に短いメッセージが出ることがあります。カウンターを非表示にすると、
メッセージも表示されません。

  REW         巻き戻し中
  FF          早送り中、または次のラウンドを検索中
  TAKEOVER    キャラクターを操作中
  REPLAY      録画されたリプレイに復帰
  NO CKPT     ここから戻れる保存地点がありません
  LAST ROUND  これより後のラウンドはありません
  ROUND 1     すでに最初のラウンドの先頭です
  F8 ON       先にゲームのF8ポーズを解除してください

テイクオーバー (操作の引き継ぎ)
-------------------------------
Revival では、リプレイ再生中に方向キーまたはゲームのボタンを押すとキャラクターを
操作できます。Revival の SwitchPlayers キー (初期設定はテンキーの 0) を押さない限り、
1P の操作を受け付けます。Revival で録画されたリプレイが必要です。

このModを使うと、リプレイに戻るために事前にセーブしておく必要はありません。

- 「-」キー (または Revival のリプレイ戻る / 進むキー。初期設定はテンキーの - と +) を
  短押しすると、操作を始めた時点に戻り、リプレイの続きが通常どおり再生されます。
  長押しするとそのまま巻き戻しが続きます。
- Backspace でラウンドの先頭へ、\ で次のラウンドへ移動します。
- KOの少し後、試合が終わったとき、録画が最後まで進んだときにも自動で戻ります。
- テイクオーバー中の P は1フレーム進んで一時停止します。操作に使っているキーは
  巻き戻しキーとして働きません。

注意事項
--------
- 巻き戻し、早送り、ラウンドスキップには EfzRevival 1.02e、1.02f、1.02g、1.02h、
  1.02i、1.02j のいずれかが必要です。それ以外のバージョンではこれらのキーは自動で
  無効になり、オプションの「巻き戻しキー」も表示されません。
- メニューは、通常版 EFZ と新旧の Revival で録画されたリプレイを読み込めます。
  再生そのものはゲームと Revival が行います。
- 巻き戻せるのは、再生済みの範囲のうち最大で約19,000フレームです。
- これらのキーを無効にするには、DLL と同じ場所の ImprovedReplayMenu.ini に次の内容を
  追加してください (ファイルがなければ作成します)。

  [Rewind]
  Enabled=0

- 設定は DLL と同じ場所の ImprovedReplayMenu.ini に保存されます。
- キーが反応しない場合は、他のModがそのキーを使っている可能性があります。
  オプションの「巻き戻しキー」で別のキーを選んでください。
