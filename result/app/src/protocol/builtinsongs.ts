// The CA79's built-in songs (Music tab: Lesson, Concert Magic, Piano Music) as a list a person can pick from.
//
// Where they are: NOT in the piano. The main board's firmware has no song library - its SongSelect (10 52 00) reaches
// only the 10 recorder songs (numbers >= 10 become song 1) and its Music commands do nothing in MusicMode 4 / 6.
// The pieces are Standard MIDI Files inside the panel's Kawai app (the same app as the phone's "PianoRemote"); that
// app plays them itself and sends the notes to the piano as MIDI. So a piece can sound only where that app's files
// are: on the panel, through its shell ("kwmcore" bridge). The shell lists its files (getSmfList) and plays one
// (selectSmf + playStartSmf) - nothing of those files is here, not even their names.
//
// A piece is found by a key: FNV-1a (32 bit, base 36) of the path the shell's getSmfList gives for it. At run time
// the app hashes what the shell lists and plays the matching path. Titles and composers are plain facts written for
// this app. Regions: the CA79's EU and US lists differ in Concert Magic and Piano Music (the US also has hymns);
// "^" marks a piece of the EU list only, "!" of the US list only.

export type BuiltinMode = "lesson" | "cm" | "piano";
export type Region = "eu" | "us";

export interface BuiltinSong {
  title: string;
  /** "" when there is none to name (traditional, an exercise) */
  composer: string;
  /** FNV-1a 32 (base 36) of the shell's path for the piece */
  key: string;
  /** only in that region's list; undefined = both */
  region?: Region;
}

export interface BuiltinBook {
  /** stable key (also the select value) */
  id: string;
  title: string;
  /** the pieces have a right-hand and a left-hand part the piano can play louder or softer (hand practice) */
  hands: boolean;
  songs: BuiltinSong[];
}

/** FNV-1a 32 of a string's UTF-8 bytes (the shell's paths are ASCII), base 36. */
export function songKey(path: string): string {
  let h = 0x811c9dc5;
  const b = unescape(encodeURIComponent(path));
  for (let i = 0; i < b.length; i++) {
    h ^= b.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h.toString(36);
}

const pad = (n: number, w: number) => String(n).padStart(w, "0");
const range = (a: number, b: number) => Array.from({ length: b - a + 1 }, (_, i) => a + i);
const book = (id: string, title: string, songs: BuiltinSong[], hands = true): BuiltinBook => ({ id, title, hands, songs });
const flagged = (k: string): { key: string; region?: Region } =>
  k[0] === "^" ? { key: k.slice(1), region: "eu" } : k[0] === "!" ? { key: k.slice(1), region: "us" } : { key: k };
/** "key~Title~Composer|…" (composer optional) */
const unpack = (s: string): BuiltinSong[] =>
  s.split("|").map((e) => {
    const [k, title, c] = e.split("~");
    return { ...flagged(k), title, composer: c ?? "" };
  });
/** keys in order + a title for each position */
const titled = (keys: string, title: (i: number) => string, composer: string | ((i: number) => string)): BuiltinSong[] =>
  keys.split("|").map((k, i) => ({ ...flagged(k), title: title(i), composer: typeof composer === "string" ? composer : composer(i) }));

// ---- Lesson books ----

const beyerTitle = (i: number) => (i < 13 ? (i ? `Theme 1 · Var. ${i}` : "Theme 1") : i < 22 ? (i > 13 ? `Theme 2 · Var. ${i - 13}` : "Theme 2") : `No. ${i - 19}`);

// Op. 100 - the usual French titles
const BURGMULLER = "La candeur|Arabesque|Pastorale|Petite réunion|Innocence|Progrès|Le courant limpide|La gracieuse|La chasse|Tendre fleur|La bergeronnette|Adieu|Consolation|La styrienne|Ballade|Douce plainte|La babillarde|Inquiétude|Ave Maria|Tarentelle|L'harmonie des anges|Barcarolle|Le retour|L'hirondelle|La chevaleresque".split("|");
const INV_KEYS = ["C major", "C minor", "D major", "D minor", "E♭ major", "E major", "E minor", "F major", "F minor", "G major", "G minor", "A major", "A minor", "B♭ major", "B minor"];

// Sonatina Album 1: [number, composer, work, movements]
const SONATINAS: [number, string, string, number][] = [
  [1, "Kuhlau", "Op. 20 No. 1", 3], [2, "Kuhlau", "Op. 20 No. 2", 3], [3, "Kuhlau", "Op. 20 No. 3", 3],
  [4, "Kuhlau", "Op. 55 No. 1", 2], [5, "Kuhlau", "Op. 55 No. 2", 3], [6, "Kuhlau", "Op. 55 No. 3", 2],
  [7, "Clementi", "Op. 36 No. 1", 3], [8, "Clementi", "Op. 36 No. 2", 3], [9, "Clementi", "Op. 36 No. 3", 3],
  [10, "Clementi", "Op. 36 No. 4", 3], [11, "Clementi", "Op. 36 No. 5", 3], [12, "Clementi", "Op. 36 No. 6", 2],
  [13, "Haydn", "Hob. XVI:35", 3], [14, "Mozart", "K. 545", 3], [15, "Beethoven", "Op. 49 No. 2", 2],
  [16, "Beethoven", "Op. 49 No. 1", 2], [17, "Dussek", "Op. 20 No. 1", 2], [18, "J. S. Bach", "Prelude in C major, BWV 846", 1],
  [19, "Haydn", "Adagio", 1], [20, "Haydn", "Andante", 1], [21, "Haydn", "Allegro", 1], [22, "Haydn", "Andante", 1],
  [23, "Mozart", "Rondo, K. 485", 1], [24, "Beethoven", "Rondo, Op. 51 No. 1", 1], [25, "Beethoven", "Andante", 1],
  [26, "Schubert", "Op. 142 No. 3", 1], [27, "Schubert", "Scherzo", 1], [28, "Schubert", "Op. 120", 1],
  [29, "Weber", "Op. 7", 1], [30, "Mendelssohn", "Op. 16 No. 1", 3],
];
const ROMAN = ["I", "II", "III"];
// (no flatMap: the panel's Chrome 71 has none)
const SONATINA_ROWS: [string, string][] = [];
for (const [n, composer, work, mv] of SONATINAS) {
  if (mv === 1) SONATINA_ROWS.push([`${n}. ${work}`, composer]);
  else for (const m of range(1, mv)) SONATINA_ROWS.push([`${n}. ${work} · ${ROMAN[m - 1]}`, composer]);
}

const WALTZ_WORKS = ["Op. 18", "Op. 34 No. 1", "Op. 34 No. 2", "Op. 34 No. 3", "Op. 42", "Op. 64 No. 1", "Op. 64 No. 2", "Op. 64 No. 3", "Op. 69 No. 1", "Op. 69 No. 2", "Op. 70 No. 1", "Op. 70 No. 2", "Op. 70 No. 3", "KK IVa/15", "KK IVa/12", "KK IVa/13", "KK IVa/14", "KK IVb/10", "KK IVb/11"];

// finger exercises: 12 keys each, C to B
const MAJOR = ["C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"];
const MINOR = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "G♯", "A", "B♭", "B"];
const exercise = (id: string, title: string, keys: string, minor: boolean): BuiltinBook =>
  book(id, title, titled(keys, (i) => `${(minor ? MINOR : MAJOR)[i]} ${minor ? "minor" : "major"}`, ""), false);

export const LESSON_BOOKS: BuiltinBook[] = [
  book("beyer", "Beyer: Elementary Method, Op. 101", titled("1ue9ytk|1yx1rkb|17hcfca|br4dx9|1e1drvw|liepqn|o2lg2m|2ha90h|1hvhf6o|oqv68z|dxcn0t|175ihdm|1s6k4kb|1vi9y97|1rvalm0|1ioafx|142km16|p4epin|1amlyks|1ryn4ox|eiwhdq|m7tlfn|1k4x3hj|m4gm2q|1j0obpx|kd95ds|1xz7ooz|1qaakn2|14ox5z5|dlwxh7|7rtapk|163a86l|1m5r42i|ifiu2n|1sm9syk|f23b9t|1j0t6ry|11u9owj|69a21c|1mvk02o|11geher|1hsyrdu|ndj5bp|197f39g|nm3w7b|4pygm|12tj4t5|7dy37s|4w0ah7|1xavb2d|thefzm|spw377|1vyz700|vead0p|1hlayh2|fqib6f|1pu5c2c|qmvy6l|1cuc722|xvx9oy|1vkers5|284780|x4cpar|1f63aee|tnttqh|1nexnzo|jg7shj|1b3vnru|ucnn3h|1ojfx6v|b3n29w|4pr3tl|1121zg6|3cmgf7|orrz34|1oav6b9|jp6qea|45zwv|lmfgks|woawvg|45buq7|6pil26|1k5bfz5|1d173t4|1hjywjv|q49kbu|1tf5kvx|mu9nd0|18w8p3|1bkyq5t|fms3la|1l3becf|q3yy9o|1vldtzh|psfmd6|1ath9jv|14zdms8|20fbmx|niosau|kx15se|1fzfc9d|uihrz0|1jcuv27|o1z32y|7zi771|18p5bp4|1ej8ygr|ssvmw6|9yscp|1ilcnw3|tu3isw|v5xf9h|1io63hu|14x7r2v|fzqpzo|1l9t5bd|7qyk06|v1r197|p7nehk|1hdgx35|3xq9ry|ej8hwv|101fqz0|1pym4t9|theefe|1kx3qnf", beyerTitle, "Beyer")),
  book("burgmuller", "Burgmüller: 25 Études, Op. 100", titled("sd87xr|hrpzsu|1v7gn41|1v44414|1yr3gob|17be4ga|4rhsv1|xweuhw|bmcinb|15kmsep|ai8lxq|18y2b7j|k3p84c|1wltpbh|dn6j8a|144gem3|yacrug|1owaah5|ie331i|189djn6|kr4vet|jfayy8|19ghhnc|1wda24m|1av0lgp", (i) => `${i + 1}. ${BURGMULLER[i]}`, "Burgmüller")),
  book("czerny100", "Czerny: 100 Studies, Op. 139", titled("4q5lcw|1498sl1|1yooen6|14htjgn|1q34qis|1jp8s2h|h0flpy|lrpxqj|o9nqh4|j6pem6|1wmi9j5|18yimq4|mri19r|19f6vnu|1yvmsyl|1pidxd4|15114b|1veur2u|l83s6x|1bjrg99|5qt8my|qruvtn|kxr920|rjccfl|1um9ru6|111p0m7|62ckjg|qw19tx|1ee9y2a|szkq2w|xicitn|62n6lm|19dj75p|cmoj58|1j4tiz3|1lp09b2|103p28x|ggs6g0|1md9zhf|l11hzj|1dk0k4s|1zx19d|nl88bi|1yfojt7|1twwr2g|b9r665|16zz7l6|i4lvyf|13pzamc|40cpcq|1myzvfx|onixyw|uhmkqj|ves26|vxsyj5|agve8s|zb8hbz|1ptid8y|ogtlr9|a68bw9|1kczas6|1ziorr|o9l0mc|v9x05|13069c2|ftt9z7|118ysn4|1lkmt8h|84w5xa|7oqxck|wm7yfr|1yh0lqe|1ca00a1|dtksa8|19llqgj|1ad438y|f5gwcl|1tcw918|pzvrxb|z8t23v|ol1wdk|xyarz1|8huuoa|1kva29b|816lqk|vp68jl|1hahflq|1hyr5s3|51jr9c|11kufk6|g2kyw9|19tmljo|1yr3mmv|dgxx2q|1oztath|1nnzecw|de4hgz|toxqum|1pn4df5|nplry0", (i) => `No. ${i + 1}`, "Czerny")),
  book("czerny30", "Czerny: 30 Studies, Op. 849", titled("khzl8q|1b8xsob|10l6my0|17paz41|1tam666|1wvestr|k1bcb0|1mpr21h|i42m4i|1v1o0m0|1unlej|1fuq06y|mmk5u5|1g69c3g|c4hq73|15p2hf2|2m520h|n02y8|pe981f|koeg1f|1nxhju8|1p9dnwl|lfwstu|7p0o0n|1hsnowk|ncspux|19jtbba|1p552aj|1qbt8m0|1fp6bm6", (i) => `No. ${i + 1}`, "Czerny")),
  book("inventions", "J. S. Bach: Inventions", titled("s37qtw|1xda65l|jufkue|1uotoqb|15xkjn4|179eg3p|1urn4c2|1hf26vz|mfpqt8|kquijm|1pciygl|wu5cps|3pj3s3|194fd2u|52nr6h", (i) => `Invention No. ${i + 1} in ${INV_KEYS[i]}, BWV ${772 + i}`, "J. S. Bach")),
  book("sonatinas", "Sonatina Album 1", titled("j6hyzu|12wqky3|15eodoo|12ohcjb|1yeamue|15vdsax|1pvk1x8|d04p0x|1n3rpwu|r88bm9|5rarbw|1p65x5i|gomuf|t2rmzo|1tivub7|1uae73m|9j3ut4|1051uel|1gw9v2y|1w4dqod|14tomag|13n0fyz|1fav3he|y9jopv|1re5xnk|u3i5m4|1hkiopt|44ptsu|fgshyf|1xu93yu|nni52x|1s9yamy|jq441n|1d93jut|1bx7fsg|8o4bzn|1vc4ns8|4zicm5|17jeo7e|1dych0j|1uawqzm|17yfv46|63l07r|dtze81|1p73te4|66k5xe|1as8lud|g5799l|11ngpxi|1ytpx07|19w8vx0|1p2fl6t|djk7g2|dgqrub|1nqloq8|1ppqnsh|trk17y|1sfupba|wq1f07|1tnzhf8", (i) => SONATINA_ROWS[i][0], (i) => SONATINA_ROWS[i][1])),
  book("waltzes", "Chopin: Waltzes", titled("11kb4d2|1i7ykbr|5gwu78|1h3psk5|k7i2wy|1w295j7|igam80|12rymtd|1odc1ha|1nx6swk|dtjs0n|1focfba|thbtux|1u20nu8|qsxk1f|rkfwtu|1vdwrwl|1ak82m4|777li7", (i) => `Waltz No. ${i + 1}, ${WALTZ_WORKS[i]}`, "Chopin")),
  book("etudes", "Chopin: Études, Op. 10", titled("1klpwsk|1e7tyc9|bj0rzq|1cupaxv|1y9utls|yrtyut|1t79kwy|19m8ufj|1v4ib3g|8cw28i|161dkbp|1bq71qo", (i) => `Étude Op. 10 No. ${i + 1}`, "Chopin")),
  // Alfred's Basic Piano Library / Premier Piano Course (titles as in the books)
  book("alfred-1a", "Alfred: Basic Piano Library, Lesson Book 1A", unpack("1x3mjfl~Right & Left|2hcboj~Left & Right|8k8v7k~Merrily We Roll Along/O'er the Deep Blue Sea|1fdp9e5~Hand-Bells|dautca~Jolly Old Saint Nicholas|1ctouj~Old MacDonald|1rd9q65~Batter Up!|4xsksr~My Clever Pup|tgoi21~The Zoo|lj86j2~Playing in a New Position|a8w1c~Sailing|9uueji~Skating|1mb4n30~Wishing Well|wijhrw~Rain, Rain!|1m7e59u~A Happy Song|1tjg28f~Position C|ms4ehd~A Happy Song|154qlob~See-Saws|108wcp~Just a Second!|5ifbwg~Balloons|1vuj2ua~Who's on Third?|11yyhfo~Mexican Hat Dance|pg6tig~Rock Song|13pp5dk~Rockets|11akkkc~Sea Divers|4wos8k~Play a Fourth|1hb6en4~July the Fourth!|1eb39bk~Old Uncle Bill|13fxnak~Love Somebody|1lz1nwx~My Fifth|k81idd~The Donkey|1vjl7h8~Position G|4837h0~Jingle Bells!|1mmdmhw~Willie & Tillie|17st913~A Friend Like You|13qbtud~My Robot|1i27znf~Rockin' Tune|180ipud~Indian Song|zf8bpj~Raindrops|wmg3uy~It's Halloween!|eyn0qj~Horse Sense")),
  book("alfred-1b", "Alfred: Basic Piano Library, Lesson Book 1B", unpack("9srxs0~Step Right Up!|1b2zrck~The Carousel|t7qlz1~Hail to Thee, America!|hsexqx~Brother John|hlhlq1~Good Sounds|1hdtue3~The Cuckoo|1k77gl9~Money Can't Buy Ev'rything!|1wsud6z~Ping-Pong|1kfxg42~Grandpa's Clock|10q3r59~When the Saints Go Marching In|vovewn~G's in the \\\"Bag\\\"|1l76lyx~Join the Fun|1tbh0mf~Oom-Pa-pa!|1o3562d~The Clown|10utifz~Thumbs on C!|1mkcdq1~Waltz Time|bgca6n~Good King Wenceslas|dhdk7h~The Rainbow|xk3i5v~Yankee Doodle|11q2pst~The Windmill|17j2c7j~Indians|1609lst~New Position G|1qwugk3~Pedal Play|1nd9uit~Harp Song|1q7x8w7~Concert Time|mnrctx~Music Box Rock|1jvryki~A Cowboy's Song|87req6~The Magic Man|17nset1~The Greatest Show on Earth!|gokyw1~The Whirlwind|qoihk2~The Planets|1rwy2mp~C Major Scale Piece|sex2lj~G Major Scale Piece|bj8udy~Carol in G Major|n9x7jk~The Same Carol in C Major|10chuoe~French Lullaby|sk5i2s~Sonatina|zeoo1q~When Our Band Goes Marching By!")),
  book("alfred-adult", "Alfred: Adult All-in-One Course", unpack("cjvn3e~Ode To Joy (Theme from Beethoven's 9th Symphony)|13zkzbf~Aura Lee|zgt6ko~Rock-Along|16kxiqp~Mexican Hat Dance|1s68psu~Au clair de la lune|1pm1zgv~Tisket, A Tasket|j3wzn0~Rockin' Intervals|1fge8ol~Good King Wenceslas|aupsrm~My Fifth|1t8jfg8~Jingle Bells|1j08r~Brother John|18ld6u2~Here's A Happy Song!|siway5~Merrily We Roll Along|1f1vvq4~Largo (from \\\"The New World\\\")|4v4wu7~Mary Ann|15gf9ji~Rockets|1hpe1d~What Can I Share?|1rq06s0~When The Saints Go Marching In|o9vro3~When The Saints Go Marching In|jjys2b~Love Somebody!|1noubyo~A Friend Like You|1i00ujp~The Donkey|kbjcgi~Money Can't Buy Ev'rything!|5vtv93~The Cuckoo|19ujqr8~Harp Song|g3fwi1~Beautiful Brown Eyes|11lpd5y~Alpine Melody|1v1h7ej~Thumbs on C!|1p7fs8o~Waltz Time|cjl118~Standing In The Need Of Prayer|17ixh3z~The Gift To Be Simple|c81p4q~Alouette|1i0zwr1~Lavender's Blue|rezpjs~Kum-Ba-Yah!|x93cbf~London Bridge|9yav2e~Michael, Row The Boat Ashore|1nh5gdl~Blow The Man Down!|1ir8grt~Lone Star Waltz|5x5092~Café Vienna|133gexj~Rock It Away!|e5zduc~Joy To The World|lmpcud~Cockles And Mussels|1gucjqq~Got Those Blues!|1g2u6yb~On Top Of Old Smoky|kat8s0~The Can-Can|mypcmp~The Marine's Hymn|1x2cdim~Why Am I Blue?|1wkd70k~Little Brown Jug|1az1zyf~Chiapanecas|nho27q~O Sole Mio!|1q6h8k9~Jericho|b7e1uo~Greensleeves|1otcl5v~Go Down, Moses|64st5u~Scarborough Fair|1aqh92t~Raisins And Almonds|821jcc~He's Got The Whole World In His Hands|1lkw4nj~The Entertainer|15sgxtr~Amazing Grace")),
  book("alfred-p1a", "Alfred: Premier Piano Course 1A", unpack("lax84m~Steady Quarter Notes|1om5s6v~Our Journey|tjrlpw~Treasure Map|11p8pid~Treasure Chest|40r7f6~Practice Carefully|396n0z~It's Fun to Play!|17e26xc~Taking Turns|19682a9~Great News|e3ro7i~Dream Big Dreams|17tyl2c~Merrily We Roll Along|f7vkx3~Old MacDonald Had a Dog|4w8t92~Fortune Cookies|tqlwc9~Climbing the Music Ladder|1ehdwe8~Up the Attic Stairs|sdh8xv~Down the Attic Stairs|1fjs0oy~Old MacDonald Had a Mouse|l4cemt~Rock Wall|1hh0qvg~Climbing Down|va05f3~A Jazzy Tune|g6c3dr~Hush, Little Baby|1jjckho~Let's Take a Trip|6nx7ld~Big Ben|1grk8ha~Gum Ball Machine|57om6z~Early to Bed|6ecsig~Change on C|xp1wwd~Arrowhead|1jwi5ru~My New Piece|mso9w7~Waltzing|1wwbas4~Aspen Trees|1r91p7u~Ice Pops|ntf54t~Twinkling Planets|eg41xk~Skating|by696z~Basketball|17ucxjy~French Fries|ejism9~Skateboard Champ|1qsdga4~Minuet for Bach|15a3zm7~Alouette|1ov4q3m~My Kite|ufp41h~All-Star Game|jewbfh~Ode to Joy|1lysn0q~A Page or Two|eddx9n~Snowy Day|aqekmg~Bike Ride|atr3pd~Old Joe Clark|wf4ida~Hopscotch|170mqi7~Rise and Shine!|1sitzkc~The Wheels on the Bus|p5i9g5~Eine Kleine Mozart|1raels2~Haydn's Surprise|14i4ti0~Time to Celebrate")),
  book("alfred-p1b", "Alfred: Premier Piano Course 1B", unpack("ty7fj3~The Boat Dock|1nis6r2~At the Park|kfurch~Walk to School|du9nyw~Snorkeling|jodaqj~Opening Day|1xofpiy~Green Tea|14g9v65~The Library|4mczh0~Smoothies|tju0k7~Crispy Chips|1pj0og1~In the Pool|c37tj2~Catch That Frog!|fo0g6n~My Pony|11v11n0~Popcorn|1rs57vh~Chopsticks|12bpakq~C, Over and Over|1t2ni0b~Row, Row, Row Your Boat|1iewca0~Picnic Ants|e4m73t~Butterfly|1obd5zq~G, Over and Over|1rl6tk2~Symphony Hall|x5r7hx~Sara's Musette|1wns28w~My Yo-Yo|1b8mjkz~In Old Hong Kong|9wy0mu~Russian Folk Tale|1clr6zd~Wind Chimes|1izn5fo~The Bells of St. Joseph|xebydj~Hot Air Balloon|1tz3soa~Creepy Crawler|qjh8l9~I Asked My Mother|piqdcn~Broadway Star|l9c9g~The Joke|156if6x~My Sombrero|1rdj0na~The Mythical Unicorn|12i45df~Lunch Box Blues|6q3774~Egyptian Pyramids|81zb9h~It's a Brand-New Day!")),
  book("hanon", "Hanon: The Virtuoso Pianist", titled("66sm4g|14u7sgl|7y02te|15ygk87|1s8iw2s|1e567cp|pat49i|u23ga3|qf43mw|49omvy|19zyjxd|n7p5m4|1pfoy7|nodeju|1j9uwfx|19wjt8o|17em0i3|12nbohi|6b0sux|vxxc4t", (i) => `No. ${i + 1}`, "Hanon")),
  exercise("scale-maj", "Scales (major)", "neccdm|1ejpq4y|131ur9n|69lg6d|7ytnuk|70stol|hkp18n|wne2jq|1sv90q9|lp7s40|1m792qw|1k4adfd", false),
  exercise("scale-nmin", "Scales (natural minor)", "1qj5n6b|1ikput3|8xzaga|18r85og|1meedoh|15elrkg|1kbwfgk|17yiuxj|s1fh6b|1degvf1|1dyh2x1|1mmf0z0", true),
  exercise("scale-hmin", "Scales (harmonic minor)", "l9uxjx|xirwch|t0a7fk|1jyzkha|b1khev|vav8oq|1anvffa|1wvfa35|1tjyj7x|dgpbqb|slhdhr|8qqdl2", true),
  exercise("scale-mmin", "Scales (melodic minor)", "fg5uyi|rv5nkm|cegnoj|1jq64ad|17qi7wc|di4z31|b2243l|1urvlom|1dpj4y2|1tffhi8|1anjz1s|mc7501", true),
  exercise("arp-maj", "Arpeggios (major)", "1agwmdu|3wi2e2|1bw0ogp|1njx4hz|nnzmbs|1ub989z|1htxujh|ko2ip2|1p1wkt7|1chxj1g|13u7lng|1fydh9f", false),
  exercise("arp-min", "Arpeggios (minor)", "125gyzi|1q9hk62|umqd3p|1sxta8j|uxfz8s|awx9gz|u9n87r|qbwda2|1j4w85a|16iltnk|19nesco|1ktodp3", true),
  exercise("cad-maj", "Cadences (major)", "dhbfvj|1446msn|bg8tq6|2p498c|1wn867h|4bt0po|14xk59m|awwd9n|1b2oxs8|1bzodo1|1wzgnqh|1dkfgvk", false),
  exercise("cad-min", "Cadences (minor)", "1cdu2ir|12i73xj|1m2fww2|1ehc7wg|1e3teah|13uya00|1v08tv0|cw1nuv|1yoomzn|1m3nv5x|3rf8od|1jm6hbg", true),
  exercise("chord-maj", "Chords (major)", "1hvknyi|2jmbz6|ds8hrn|1jutli9|1w8q6zk|1gvgse9|k77swf|1gi2lee|ak9dbx|t5yu2s|s5kq18|1hz4xyt", false),
  exercise("chord-min", "Chords (minor)", "18i80u6|1nm9cyq|i87wrb|2vii05|yo56zg|ntfo3p|9030j5|m6tvsy|19r2g66|1f79v54|nxudcw|2aaw6p", true),
];

// ---- Piano Music (listening; the US list also has hymns) ----

export const PIANO_MUSIC: BuiltinBook[] = [
  book("piano", "Piano pieces", unpack("!1f7yc3d~Air (Suite No. 3, BWV 1068)~J. S. Bach|!1ntp27q~Prelude in C major, BWV 846~J. S. Bach|!l6lzbg~French Suite No. 1: Allemande~J. S. Bach|!g4momq~French Suite No. 3: Allemande~J. S. Bach|!1d3hd3k~French Suite No. 5: Allemande~J. S. Bach|!1ubkyd3~French Suite No. 5: Gavotte~J. S. Bach|!m89gh9~French Suite No. 6: Allemande~J. S. Bach|!1ifcpnc~English Suite No. 3: Gavotte~J. S. Bach|!cvfegh~Sonata No. 28, Op. 101 · I~Beethoven|!x246s3~Sonata No. 14 \\\"Moonlight\\\" · I~Beethoven|!145n7v6~Sonata No. 14 \\\"Moonlight\\\" · II~Beethoven|!k9ehua~Sonata No. 8 \\\"Pathétique\\\" · II~Beethoven|!1iw6b5i~Intermezzo Op. 118 No. 2~Brahms|!1sx1gz3~Waltz Op. 39 No. 15~Brahms|!10l3gi~Clair de lune (Suite bergamasque)~Debussy|!x752zc~Prélude (Suite bergamasque)~Debussy|!x7kjkx~Rêverie~Debussy|!1ew90mz~Romance sans paroles Op. 17 No. 3~Fauré|!1stj9na~Nocturne No. 10 in E minor~Field|!1jdmt8a~Nocturne No. 5 in B♭ major~Field|!19i365f~Song without Words Op. 19 No. 1~Mendelssohn|!dcm4n4~Song without Words Op. 102 No. 4~Mendelssohn|!btvj5n~Sonata K. 331 · I~Mozart|!1wiczug~Sonata K. 545 · III~Mozart|!1f89lrp~Sonata K. 283 · I~Mozart|!mpw00w~Sonata K. 283 · II~Mozart|!1smdt2b~Sonata K. 283 · III~Mozart|!mvdzke~Sonata K. 547a · II~Mozart|!26w0u7~Ave Maria, D. 839~Schubert|!1iieulw~Carnaval: Chopin~Schumann|!eq1wsh~Carnaval: Coquette~Schumann|!3y07jq~Carnaval: Réplique~Schumann|!cs6cd9~Fantasiestücke: Des Abends~Schumann|!x119c3~Kinderszenen: Bittendes Kind~Schumann|!14icluu~Kinderszenen: Träumerei~Schumann|!1ubjcoo~Kinderszenen: Fürchtenmachen~Schumann|!1arxsmt~Kinderszenen: Von fremden Ländern und Menschen~Schumann|!p0s0hv~Kinderszenen: Glückes genug~Schumann|!1tpjztz~Kinderszenen: Fast zu ernst~Schumann|!in6xpm~Kinderszenen: Kind im Einschlummern~Schumann|!1rw7kqg~Barcarolle, Op. 37a No. 6~Tchaikovsky|!1r5d1l9~Arabian Dance (The Nutcracker)~Tchaikovsky|^gt6gk0~Tambourin~Rameau|^i6frt4~The Harmonious Blacksmith~Handel|^1shiz9s~Minuet in G major, BWV Anh. 114~J. S. Bach|^kw4jt5~Minuet in G minor, BWV Anh. 115~J. S. Bach|^16ee0h2~Minuet, BWV Anh. 116~J. S. Bach|^1mrryhn~Le coucou~Daquin|^gisx0d~Gavotte~Gossec|^reupm9~Minuet~Boccherini|^5jg7zd~Theme and Variations (Sonata K. 331)~Mozart|^mr7y6w~Turkish March~Mozart|^1ff7cxz~Minuet~Mozart|^ngzsgc~Sonata No. 14 \\\\|^1ysje7~Sonata No. 8 \\\\|^1faaf7x~Für Elise~Beethoven|^12rv109~Rondo favori~Hummel|^1egh462~Impromptu Op. 90 No. 4~Schubert|^hf5rkb~Moment musical Op. 94 No. 3~Schubert|^5c3q6z~Entr'acte (Rosamunde)~Schubert|^1rxxi6~Impromptu Op. 142 No. 3~Schubert|^ppo31i~On Wings of Song~Mendelssohn|^xs6fu8~Spring Song~Mendelssohn|^1iz6vrg~Rondo capriccioso~Mendelssohn|^15bbinm~Étude Op. 10 No. 3~Chopin|^oygayl~Prelude Op. 28 No. 15 \\\\|^1irigg7~Waltz Op. 64 No. 1 \\\\|^174ckv0~Nocturne Op. 9 No. 2~Chopin|^us7jyt~Fantaisie-Impromptu~Chopin|^tk5yp8~Polonaise Op. 40 No. 1 \\\\|^116wjz~Polonaise Op. 53 \\\\"), false),
  book("hymns", "Hymns", unpack("!1q6lsoj~A Mighty Fortress|!9norly~Abide With Me|!bfgpra~All Hail the Power of Jesus Name|!7i884y~Amazing Grace|!1bisuqx~Ave Maria (Schubert)|!1khelhh~Be Still, My Soul|!a5fnyu~Be Thou My Vision|!eb7nh4~Blessed Assurance, Jesus is Mine|!1kdsjvs~Blessed Be the Name|!1anizw9~Blest Be the Tie That Binds|!d3mvi0~Break Thou the Bread of Life|!x0g0v9~Come Thou Fount of Every Blessing|!19u0ux4~Come, Holy Ghost|!bqarcp~Come, Thou Almighty King|!1w6zz40~Crown Him with Many Crowns|!z0igwm~Doxology|!7gd91e~Fairest Lord Jesus|!1o49ftp~Faith of Our Fathers|!1bksxhf~For the Beauty of the Earth|!34bkrf~Gloria Patri|!1ugqgzf~God Be with You 'Til We Meet Again|!1sz1ye9~Great is Thy Faithfulness|!6hf6jq~Have Thine Own Way, Lord|!amdwzv~Holy God, We Praise Thy Name|!14gto51~Holy, Holy, Holy|!c7xnhe~How Firm a Foundation|!1wftjx~How Great Thou Art|!16uggyg~I Heard the Voice of Jesus Say|!uyvfyw~I Need Thee Every Hour|!1bji2rn~I Surrender All|!8it7si~It is Well With My Soul|!s9a4sk~Jesus Loves Me, This I Know|!1vxfrvs~Jesus Loves the Little Children|!r349aj~Joyful, Joyful, We Adore Thee|!1hlfwie~Just As I Am, Without One Plea|!10mb84e~Lead On, O King Eternal|!hf4ehx~Leaning on the Everlasting Arms|!16lj0z6~Love Divine All Loves Excelling|!ygijns~My Jesus, I Love Thee|!1u5y75v~My Savior's Love (I Stand Amazed)|!f9y7p4~Nothing But the Blood|!we5dmj~O God Our Help in Ages Past|!jejrmy~O How I Love Jesus|!1cttor1~O Worship the King|!1bp4el3~Onward, Christian Soldiers|!80g9yz~Praise to the Lord, the Almighty|!1l3png2~Rejoice, the Lord is King|!1vtbb8w~Rejoice, Ye Pure in Heart|!39w3e9~Rock of Ages|!15iv7h8~Savior, Like a Shepherd Lead Us|!1obzygy~Spirit of the Living God|!1uckd7~Stand Up, Stand Up for Jesus|!1u2waya~Standing on the Promises|!g8y40x~Sweet Hour of Prayer|!n3wrmk~Take My Life and Let It Be Consecrated|!v0vz67~Take Time to Be Holy|!1xv1km6~The Church's One Foundation|!3grnq2~The King of Love My Shepherd|!1dsv4hr~The Old Rugged Cross|!16g0oqa~The Solid Rock|!4n55ys~This is My Father's World|!1dlqave~To God be the Glory|!1jwoxkh~Trust and Obey|!pqfef3~Turn Your Eyes Upon Jesus|!1tpiamw~Victory In Jesus|!4j152q~What A Friend We Have in Jesus|!ff8dy6~When I Survey the Wondrous Cross|!mxubcy~When We All Get To Heaven|!1x0ns9r~Wonderful Grace of Jesus|!1n2vsop~Angels We Have Heard On High|!1cghbdj~Away in a Manger|!179tqr6~Hark the Herald Angels Sing|!1y0wonj~It Came Upon the Midnight Clear|!a7458r~Joy to the World|!lwyuoj~O Come, All Ye Faithful|!1nxe70c~O Little Town of Bethlehem|!1pg8qyw~Silent Night|!1uwu2o2~The First Noel|!yrgg7r~We Three Kings|!1t41o10~Christ the Lord is Risen Today|!1no8pj~He Lives|!1i4drti~Come Ye Thankful People, Come|!1lq44ro~Let All Things Now Living|!12b9eqj~We Gather Together|!hl8vxo~America the Beautiful|!1bqyodn~Battle Hymn of the Republic|!1uvazjb~Bridal Chorus|!d88qiy~Wedding March"), false),
];

// ---- Concert Magic (groups as on the instrument) ----

export const CONCERT_MAGIC: BuiltinBook[] = [
  book("cm-children", "Children's songs", unpack("df5594~Twinkle, Twinkle, Little Star|39tsse~I'm a Little Teapot|1jxtahj~This Old Man|ko3pfp~Mary Had a Little Lamb|1kxw1k4~London Bridge|znfd61~Row, Row, Row Your Boat|1st46jd~Hickory Dickory Dock|19okw7f~Pop Goes the Weasel|!19wv662~Good Morning to You~Hill|8rl66l~Frère Jacques|8igb7j~The Farmer in the Dell|jumlbt~Bingo|!pwix3y~Itsy Bitsy Spider|f05vxp~Where, O Where Has My Little Dog Gone?|!cbmnqx~The Muffin Man|1q1lgxa~Three Blind Mice|rgj7j3~Old MacDonald Had a Farm|135o4hg~Did You Ever See a Lassie?|x7w8dy~Ten Little Indians|1dt2le1~Lullaby~Brahms|1f6qo5x~Rock-a-bye Baby|fur7sr~Here We Go Round the Mulberry Bush|1nysqr0~Little Brown Jug|1x0wr67~She'll Be Coming Round the Mountain|z3zyd6~The Whistler and His Dog~Pryor|keba9a~Polly Wolly Doodle|^6vshur~The Harmonious Blacksmith~Handel|^1ehq0xw~The Happy Farmer~Schumann|^ili5u~Lightly Row")),
  book("cm-christmas", "Christmas songs", unpack("wwarj~Hark! The Herald Angels Sing~Mendelssohn|1aaz6w6~Jingle Bells~Pierpont|1fhbh1y~Deck the Halls|1i87cyd~O Come, All Ye Faithful|100bnwy~Joy to the World|8saebh~The First Noel|xl4wr9~Silent Night~Gruber|4adeyq~We Wish You a Merry Christmas|18rn9d8~What Child Is This? (Greensleeves)|fgt5aj~Angels We Have Heard on High|kkpv6s~It Came Upon the Midnight Clear|!vpeez4~Away in a Manger~Kirkpatrick|f99ztu~O Holy Night~Adam|apyc2a~O Tannenbaum|fb5gdb~The Twelve Days of Christmas|!9ujeq6~We Three Kings~Hopkins|h2sg4d~O Little Town of Bethlehem|1ks2257~Ave Maria~Schubert")),
  book("cm-patriotic", "Patriotic songs", unpack("unhk5a~My Country, 'Tis of Thee|li5oug~Battle Hymn of the Republic|1a9shck~America the Beautiful~Ward|1vdogr3~Yankee Doodle|12i087e~Hail to the Chief~Sanderson|1aonxee~The Washington Post~Sousa|1ulmox0~You're a Grand Old Flag~Cohan|7u9497~The Stars and Stripes Forever~Sousa|!2dpxwy~Anchors Aweigh~Zimmermann|lpirb7~Under the Double Eagle~J. F. Wagner|^1bxkkjl~The Moldau~Smetana")),
  book("cm-american", "American classics", unpack("phdwf1~Londonderry Air|h5ct1t~Down in the Valley|1ozw6g6~Let Me Call You Sweetheart~Friedman|163mbf2~Home! Sweet Home!~Bishop|!1qeaeit~My Bonnie Lies over the Ocean|2dmcwo~In the Good Old Summertime~Evans|19a954s~For He's a Jolly Good Fellow|!1y931qv~Bill Bailey, Won't You Please Come Home~Cannon|12muqro~Give My Regards to Broadway~Cohan|187s4ni~Clementine|t9mmlp~Fascination~Marchetti|boxxkz~Home on the Range|!1hklb52~Take Me Out to the Ball Game~Von Tilzer|1z2b28~Auld Lang Syne|gy9j5f~Michael, Row the Boat Ashore|1uxevkd~Oh! Susanna~Foster|13ou21j~On Top of Old Smoky|!rmecmr~Daisy Bell (Bicycle Built for Two)~Dacre|zspnh3~Camptown Races~Foster|!1khybpi~The Band Played On~Ward|n3b7ye~When Johnny Comes Marching Home|7cjjbn~When the Saints Go Marching In|pbdflu~Beautiful Dreamer~Foster|t0z583~Ta-ra-ra Boom-de-ay|1kvd6l0~Turkey in the Straw|yivjy9~The Blue Bells of Scotland|1syxtus~Down by the Riverside|!1nm4czm~By the Light of the Silvery Moon~Edwards|!1ylzvcy~Shine On, Harvest Moon~Bayes|1u0jeh5~American Patrol~Meacham|fssi81~Dixie~Emmett|rq03xq~The Yellow Rose of Texas|13vdiqm~Aloha ʻOe~Liliʻuokalani|1dgdxnp~Arkansas Traveler|o3xpis~My Old Kentucky Home~Foster|10bb8y7~Red River Valley|1a3b7v7~The Entertainer~Joplin|jax4l1~Old Folks at Home~Foster|53h43~Maple Leaf Rag~Joplin|awkr01~After the Ball~Harris|11hx9e4~After You've Gone~Layton|1c2ro5m~Thunder and Blazes~Fučík|!1cgf0pz~Annie Laurie~Scott|1mqvjkg~Wabash Cannonball|1p3yzau~I've Been Working on the Railroad|vpgpnk~Grandfather's Clock~Work")),
  book("cm-hymns", "Hymns", unpack("tu2d9e~Fairest Lord Jesus|1go4y2b~Amazing Grace|atdmpq~Doxology|jv3vra~For the Beauty of the Earth|7o0jst~O Worship the King~Haydn|!va5w7x~The Old Rugged Cross~Bennard|1lo8xyg~Holy, Holy, Holy~Dykes|1dha9jd~What a Friend We Have in Jesus~Converse|hsbbml~Rock of Ages~Hastings|135ls8a~Trust and Obey~Towner|1ug29cf~Sweet Hour of Prayer~Bradbury|dzb5mx~Just as I Am~Bradbury|lee2k0~Jesus Loves the Little Children~Root|ce31ie~Hallelujah Chorus~Handel|4pb838~Jesu, Joy of Man's Desiring~J. S. Bach|10dtqnz~A Mighty Fortress Is Our God~Luther|kcyksz~Christ the Lord Is Risen Today|ewzjos~It Is Well with My Soul~Bliss|16r5dis~Crown Him with Many Crowns~Elvey|1oqvcu7~Blest Be the Tie That Binds|13iq5cv~Come, Thou Almighty King~Giardini|k5jq4~My Jesus, I Love Thee~Gordon|sntm78~The Church's One Foundation~Wesley|lzrnv7~Gloria Patri|8xhqs3~The Solid Rock~Bradbury|jkv2xt~Standing on the Promises~Carter|1wi9ens~I Need Thee Every Hour~Lowry|1r7nq91~Savior, Like a Shepherd Lead Us~Bradbury|17rv7a~Stand Up, Stand Up for Jesus~Webb|!rwyp3f~Onward, Christian Soldiers~Sullivan|li2khd~All Hail the Power of Jesus' Name~Holden|1ov3z3c~To God Be the Glory~Doane")),
  book("cm-classical", "Classical selections", unpack("17n2asd~Andante (Symphony No. 94)~Haydn|!1yc3hy6~Für Elise~Beethoven|1oxgrqt~Ode to Joy~Beethoven|1nio1ob~Clair de lune~Debussy|iyad7b~The Skaters' Waltz~Waldteufel|1jtbn6a~Peter and the Wolf~Prokofiev|1d52ort~Minuet in G~J. S. Bach|x3khbe~Romeo and Juliet~Tchaikovsky|rm7ie5~The Blue Danube~J. Strauss II|9kstws~Sleeping Beauty Waltz~Tchaikovsky|h6ywgh~Gavotte~Gossec|14nf4gn~Waltz of the Flowers~Tchaikovsky|1dvhvaa~Toreador Song (Carmen)~Bizet|ua5d38~Die Fledermaus~J. Strauss II|8f4hum~Norwegian Dance~Grieg|q0v6nr~William Tell Overture~Rossini|f3sgqd~Can-can~Offenbach|1c13xlk~Emperor Waltz~J. Strauss II|gd7xod~Marche militaire~Schubert|1181v4x~Grand March (Aida)~Verdi|1dchzgb~Gymnopédie~Satie|151b1tp~Gypsy Song (Carmen)~Bizet|f28ap7~Largo (Symphony No. 9)~Dvořák|11puirs~Prelude in A major~Chopin|m1ia12~Pizzicato Polka~J. Strauss II|e37rc6~Rondeau~Mouret|zc2er2~Voices of Spring~J. Strauss II|ypjyye~Allegro moderato~Schubert|^1odbqun~A Rag Humoresque|^1akzbt2~Waltz (Faust)~Gounod|^1taialz~Radetzky March~J. Strauss I|^1dl9pec~Åse's Death~Grieg|^d5zp94~Largo~Handel|^c49ygq~Fon-Fon|^hfwpfk~Je te veux~Satie|^rdswan~Sicilienne|^yxnf40~Serenade~Haydn|^8imztv~Melody in F~Rubinstein|^1obm6oh~Entr'acte (Rosamunde)~Schubert|^vzi2ye~Rondo on Don Giovanni~Kuhlau")),
  book("cm-occasions", "Special occasions", unpack("118fsmy~Bridal Chorus~R. Wagner|1wmzh8k~Wedding March~Mendelssohn|1tlbrl2~Pomp and Circumstance~Elgar|xyzlob~Mazel Tov")),
  book("cm-international", "International songs", unpack("1ovif8i~When Irish Eyes Are Smiling~Ball|!16umjx0~Hatikvah|oegzit~My Wild Irish Rose~Olcott|!4p7ab7~Chopsticks~de Lulli|!1ghi84z~The Star-Spangled Banner~Smith|zzx9ko~Chiapanecas|vz7h4o~Funiculì, Funiculà~Denza|!1860ssn~La Marseillaise~Rouget de Lisle|gzfq5m~La paloma~Yradier|23v67y~Santa Lucia|1q4ksit~Habanera (Carmen)~Bizet|1f7w9rr~'O sole mio~Di Capua|^12cjyz8~Romance de l'amour|^657v7f~Oh Vreneli|^vvhys5~Dark Eyes|^1b9fn4z~Die Lorelei~Silcher")),
];

/** The groups (books) of a mode as one region lists them (groups left empty are dropped). */
export function builtinBooks(mode: BuiltinMode, region: Region = "eu"): BuiltinBook[] {
  const all = mode === "lesson" ? LESSON_BOOKS : mode === "cm" ? CONCERT_MAGIC : PIANO_MUSIC;
  return all.map((b) => ({ ...b, songs: b.songs.filter((s) => !s.region || s.region === region) })).filter((b) => b.songs.length > 0);
}

/** The piano's destination code (7E 05 01 … d) as a song-list region: US = 2; EU, others and unknown = EU. */
export const regionOf = (destination: number | undefined): Region => (destination === 2 ? "us" : "eu");

/** How many pieces a mode lists in a region. */
export const builtinCount = (mode: BuiltinMode, region: Region = "eu"): number => builtinBooks(mode, region).reduce((n, b) => n + b.songs.length, 0);
