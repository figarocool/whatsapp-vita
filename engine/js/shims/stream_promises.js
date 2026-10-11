import rs from 'readable-stream';
export const pipeline = rs.promises.pipeline, finished = rs.promises.finished;
export default rs.promises;
